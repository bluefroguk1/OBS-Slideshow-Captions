#include "caption-content.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

enum class Transition { Instant = 0, Fade = 1, Obs = 3, MatchStyle = 4 };
enum class Visual { Cut, Fade, Swipe, Slide };
enum class Stage { Idle, PreviewStart, Hiding, Swapping, Showing };

const char *duration_setting(Transition transition)
{
	switch (transition) {
	case Transition::Instant:
		return "instant_duration_ms";
	case Transition::Fade:
		return "fade_duration_ms";
	case Transition::Obs:
		return "obs_duration_ms";
	case Transition::MatchStyle:
		return "style_duration_ms";
	}
	return "instant_duration_ms";
}

struct Options {
	CaptionConfig caption;
	std::string slideshow;
	Transition transition = Transition::Instant;
	bool match_duration = false;
	uint32_t duration_ms = 0;
};

using Source = std::unique_ptr<obs_source_t, decltype(&obs_source_release)>;
using SceneItem = std::unique_ptr<obs_sceneitem_t, decltype(&obs_sceneitem_release)>;
using Data = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>;

struct Job {
	CaptionConfig config;
	std::string path;
	uint64_t revision;
};

struct Reading {
	std::string caption;
	uint64_t revision;
};

struct Filter {
	explicit Filter(obs_source_t *source) : source(source), worker([this] { read_captions(); }) {}
	~Filter()
	{
		{
			std::lock_guard lock(mailbox);
			stopping = true;
		}
		wakeup.notify_one();
		worker.join();
	}

	void read_captions()
	{
		std::unique_lock lock(mailbox);
		while (true) {
			wakeup.wait(lock, [this] { return stopping || job.has_value(); });
			if (stopping)
				return;
			auto next = std::move(*job);
			job.reset();
			lock.unlock();
			std::string caption;
			try {
				caption = resolve_caption(next.config, std::filesystem::u8path(next.path));
			} catch (const std::exception &error) {
				blog(LOG_WARNING, "[slideshow-caption] %s", error.what());
			}
			lock.lock();
			if (next.revision == revision)
				reading = Reading{std::move(caption), next.revision};
		}
	}

	obs_source_t *source;
	Source slideshow{nullptr, obs_source_release};
	std::string linked_uuid;
	std::mutex mailbox;
	std::condition_variable wakeup;
	Options options;
	std::string path;
	bool has_path = false;
	bool stopping = false;
	bool options_changed = false;
	bool preview_requested = false;
	uint64_t revision = 0;
	std::optional<Job> job;
	std::optional<Reading> reading;

	// Tick and filter removal serialize transition state. Render uses the atomic state.
	std::recursive_mutex animation;
	Options current;
	Stage stage = Stage::Idle;
	std::vector<SceneItem> hidden;
	std::optional<std::string> preview_original;
	std::optional<std::string> latest_caption;
	std::string pending_caption;
	std::atomic<float> opacity{1.0f};
	std::atomic<Visual> visual{Visual::Cut};
	std::atomic<bool> incoming{false};
	uint32_t matched_show_ms = 0;
	float elapsed = 0;
	float fade_start = 1;
	float out = 0;
	float in = 0;
	bool needs_initial_caption = true;
	uint64_t animation_revision = 0;
	gs_effect_t *effect = nullptr;
	std::thread worker;
};

bool is_slideshow(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id && std::string_view(id) == "slideshow";
}

bool is_text(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	return id && (std::string_view(id) == "text_ft2_source" || std::string_view(id) == "text_gdiplus");
}

void request_caption(Filter &filter)
{
	++filter.revision;
	filter.reading.reset();
	filter.job = Job{filter.options.caption, filter.path, filter.revision};
	filter.wakeup.notify_one();
}

void slide_changed(void *opaque, calldata_t *data)
{
	auto &filter = *static_cast<Filter *>(opaque);
	const char *path = calldata_string(data, "path");
	std::lock_guard lock(filter.mailbox);
	filter.path = path ? path : "";
	filter.has_path = true;
	request_caption(filter);
}

void disconnect(Filter &filter)
{
	if (filter.slideshow) {
		signal_handler_disconnect(obs_source_get_signal_handler(filter.slideshow.get()), "slide_changed", slide_changed,
					  &filter);
		filter.slideshow.reset();
	}
}

Source text_parent(Filter &filter)
{
	Source parent(obs_source_get_ref(obs_filter_get_parent(filter.source)), obs_source_release);
	if (parent && (obs_source_removed(parent.get()) || !is_text(parent.get())))
		parent.reset();
	return parent;
}

void write_text(obs_source_t *source, const std::string &text)
{
	if (!source || obs_source_removed(source))
		return;
	Data settings(obs_source_get_settings(source), obs_data_release);
	if (obs_data_get_bool(settings.get(), "read_from_file") || obs_data_get_bool(settings.get(), "from_file"))
		return;
	if (text == obs_data_get_string(settings.get(), "text"))
		return;
	Data patch(obs_data_create(), obs_data_release);
	obs_data_set_string(patch.get(), "text", text.c_str());
	obs_source_update(source, patch.get());
}

struct TextItems {
	obs_source_t *text;
	std::vector<SceneItem> items;
};

bool collect_text_items(obs_scene_t *, obs_sceneitem_t *item, void *opaque)
{
	auto &search = *static_cast<TextItems *>(opaque);
	if (!obs_sceneitem_visible(item))
		return true;
	if (obs_sceneitem_get_source(item) == search.text) {
		obs_sceneitem_addref(item);
		search.items.emplace_back(item, obs_sceneitem_release);
	} else if (obs_sceneitem_is_group(item)) {
		obs_scene_enum_items(obs_sceneitem_group_get_scene(item), collect_text_items, &search);
	}
	return true;
}

float transition_seconds(obs_sceneitem_t *item, bool show, uint32_t matched_ms)
{
	if (!obs_sceneitem_get_transition(item, show))
		return 0;
	const auto duration = matched_ms ? matched_ms : obs_sceneitem_get_transition_duration(item, show);
	return (duration ? duration : 300) / 1000.0f;
}

bool set_visibility(obs_sceneitem_t *item, bool show, uint32_t matched_ms)
{
	const auto original = obs_sceneitem_get_transition_duration(item, show);
	if (matched_ms)
		obs_sceneitem_set_transition_duration(item, show, matched_ms);
	const bool changed = obs_sceneitem_set_visible(item, show);
	if (matched_ms)
		obs_sceneitem_set_transition_duration(item, show, original);
	return changed;
}

void restore_visibility(Filter &filter)
{
	for (auto &item : filter.hidden) {
		if (obs_sceneitem_get_scene(item.get()))
			set_visibility(item.get(), true, filter.matched_show_ms);
	}
	filter.hidden.clear();
	filter.matched_show_ms = 0;
}

void stop_animation(Filter &filter)
{
	restore_visibility(filter);
	filter.stage = Stage::Idle;
	filter.opacity = 1.0f;
}

void swap_caption(Filter &filter, obs_source_t *parent)
{
	write_text(parent, filter.pending_caption);
	filter.stage = filter.hidden.empty() ? Stage::Showing : Stage::Swapping;
	filter.elapsed = 0;
	filter.incoming = true;
	filter.opacity = filter.visual.load() != Visual::Cut && filter.in > 0.0f ? 0.0f : 1.0f;
}

void animate(Filter &filter, obs_source_t *parent, std::string caption)
{
	filter.pending_caption = std::move(caption);
	filter.elapsed = 0;
	filter.fade_start = filter.opacity.load();
	filter.incoming = false;
	filter.out = filter.in = 0.0f;
	Visual slideshow_style = Visual::Cut;
	uint32_t slideshow_ms = 0;
	if (filter.slideshow) {
		Data settings(obs_source_get_settings(filter.slideshow.get()), obs_data_release);
		const std::string_view style = obs_data_get_string(settings.get(), "transition");
		if (style == "fade")
			slideshow_style = Visual::Fade;
		else if (style == "swipe")
			slideshow_style = Visual::Swipe;
		else if (style == "slide")
			slideshow_style = Visual::Slide;
		if (slideshow_style != Visual::Cut)
			slideshow_ms = static_cast<uint32_t>(std::max<int64_t>(0, obs_data_get_int(settings.get(), "transition_speed")));
	}
	const auto transition = filter.current.transition;
	const bool match_duration = filter.current.match_duration && filter.slideshow;
	const Visual visual = transition == Transition::MatchStyle ? slideshow_style
			    : transition == Transition::Fade ? Visual::Fade : Visual::Cut;
	filter.visual = visual;
	const auto duration_ms = match_duration ? slideshow_ms : filter.current.duration_ms;
	if (transition == Transition::Obs && duration_ms > 0) {
		TextItems search{parent, {}};
		obs_enum_scenes([](void *opaque, obs_source_t *source) {
			if (obs_source_showing(source))
				obs_scene_enum_items(obs_scene_from_source(source), collect_text_items, opaque);
			return true;
		}, &search);
		const auto has_transition = [&](bool show) {
			return std::any_of(search.items.begin(), search.items.end(), [show](const SceneItem &item) {
				return obs_sceneitem_get_transition(item.get(), show) != nullptr;
			});
		};
		const bool has_hide = has_transition(false);
		const bool has_show = has_transition(true);
		const auto hide_ms = has_hide ? (has_show ? std::max(1u, duration_ms / 2) : duration_ms) : 0;
		const auto show_ms = has_show ? std::max(1u, duration_ms - hide_ms) : 0;
		for (auto &item : search.items) {
			auto *scene_item = item.get();
			const bool hide = obs_sceneitem_get_transition(scene_item, false) != nullptr;
			const bool show = obs_sceneitem_get_transition(scene_item, true) != nullptr;
			if ((!hide && !show) || !obs_sceneitem_get_scene(scene_item))
				continue;
			if (set_visibility(scene_item, false, hide_ms)) {
				filter.out = std::max(filter.out, transition_seconds(scene_item, false, hide_ms));
				filter.in = std::max(filter.in, transition_seconds(scene_item, true, show_ms));
				filter.hidden.push_back(std::move(item));
			}
		}
		filter.matched_show_ms = show_ms;
		if (filter.hidden.empty())
			filter.out = duration_ms / 2000.0f;
	} else if (visual != Visual::Cut) {
		const float total = duration_ms / 1000.0f;
		filter.out = total / 2;
		filter.in = total - filter.out;
	} else {
		filter.out = duration_ms / 2000.0f;
	}
	filter.stage = Stage::Hiding;
	if (filter.out <= 0)
		swap_caption(filter, parent);
}

void update(void *opaque, obs_data_t *settings)
{
	auto &filter = *static_cast<Filter *>(opaque);
	Options options;
	options.slideshow = obs_data_get_string(settings, "slideshow");
	options.caption.mode = static_cast<CaptionMode>(obs_data_get_int(settings, "mode"));
	options.caption.filename_part = static_cast<FilenamePart>(obs_data_get_int(settings, "filename_part"));
	options.caption.separator = obs_data_get_string(settings, "filename_separator");
	options.caption.pattern = obs_data_get_string(settings, "regex");
	options.caption.format = obs_data_get_string(settings, "format");
	options.caption.index = obs_data_get_string(settings, "index");
	options.caption.metadata = obs_data_get_string(settings, "metadata");
	options.transition = static_cast<Transition>(obs_data_get_int(settings, "transition"));
	options.match_duration = obs_data_get_bool(settings, "match_duration");
	const auto *duration_key = duration_setting(options.transition);
	options.duration_ms = static_cast<uint32_t>(std::clamp<int64_t>(obs_data_get_int(settings, duration_key), 0, 20000));
	std::lock_guard lock(filter.mailbox);
	const auto caption_fields = [](const CaptionConfig &caption) {
		return std::tie(caption.mode, caption.filename_part, caption.separator, caption.pattern, caption.format,
				caption.index, caption.metadata);
	};
	const bool refresh = filter.options.slideshow == options.slideshow && filter.has_path &&
			     caption_fields(filter.options.caption) != caption_fields(options.caption);
	filter.options = std::move(options);
	if (refresh)
		request_caption(filter);
	filter.options_changed = true;
}

void tick(void *opaque, float seconds)
{
	auto &filter = *static_cast<Filter *>(opaque);
	std::lock_guard lock_animation(filter.animation);
	auto parent = text_parent(filter);
	if (!parent || !obs_source_enabled(filter.source)) {
		stop_animation(filter);
		if (filter.preview_original)
			write_text(parent.get(), *filter.preview_original);
		filter.preview_original.reset();
		if (filter.latest_caption)
			filter.needs_initial_caption = true;
		return;
	}
	std::optional<Reading> reading;
	bool preview;
	bool changed;
	uint64_t revision;
	{
		std::lock_guard lock(filter.mailbox);
		changed = std::exchange(filter.options_changed, false);
		if (changed)
			filter.current = filter.options;
		preview = std::exchange(filter.preview_requested, false);
		revision = filter.revision;
		reading = std::exchange(filter.reading, std::nullopt);
	}
	if (changed) {
		stop_animation(filter);
		if (filter.preview_original)
			write_text(parent.get(), *filter.preview_original);
		filter.preview_original.reset();
	}
	if (filter.linked_uuid != filter.current.slideshow ||
	    (filter.slideshow && obs_source_removed(filter.slideshow.get()))) {
		const bool had_slideshow = static_cast<bool>(filter.slideshow);
		disconnect(filter);
		filter.linked_uuid = filter.current.slideshow;
		{
			std::lock_guard lock(filter.mailbox);
			filter.path.clear();
			filter.has_path = false;
			++filter.revision;
			filter.job.reset();
			filter.reading.reset();
		}
		reading.reset();
		filter.latest_caption.reset();
		stop_animation(filter);
		if (filter.preview_original)
			write_text(parent.get(), *filter.preview_original);
		filter.preview_original.reset();
		if (had_slideshow)
			write_text(parent.get(), "");
		filter.needs_initial_caption = true;
		Source source(obs_get_source_by_uuid(filter.linked_uuid.c_str()), obs_source_release);
		if (source && is_slideshow(source.get()) && !obs_source_removed(source.get())) {
			filter.slideshow = std::move(source);
			signal_handler_connect(obs_source_get_signal_handler(filter.slideshow.get()), "slide_changed",
					       slide_changed, &filter);
		}
	}
	if (reading && reading->revision == revision)
		filter.latest_caption = std::move(reading->caption);
	if (preview && !filter.preview_original) {
		stop_animation(filter);
		Data settings(obs_source_get_settings(parent.get()), obs_data_release);
		filter.preview_original = obs_data_get_string(settings.get(), "text");
		write_text(parent.get(), "Preview · Current caption");
		filter.stage = Stage::PreviewStart;
		filter.elapsed = 0;
	} else if (!filter.preview_original && filter.latest_caption &&
		   (reading || filter.needs_initial_caption)) {
		if (filter.needs_initial_caption) {
			stop_animation(filter);
			write_text(parent.get(), *filter.latest_caption);
			filter.needs_initial_caption = false;
		} else if (filter.stage == Stage::Hiding) {
			filter.pending_caption = *filter.latest_caption;
		} else {
			animate(filter, parent.get(), *filter.latest_caption);
		}
		filter.animation_revision = revision;
	}
	filter.elapsed += seconds;
	if (filter.stage == Stage::PreviewStart) {
		if (filter.elapsed >= 0.5f)
			animate(filter, parent.get(), "Preview · Next caption");
	} else if (filter.stage == Stage::Hiding) {
		if (!filter.preview_original && revision != filter.animation_revision && !reading)
			return;
		if (filter.elapsed >= filter.out) {
			swap_caption(filter, parent.get());
		} else if (filter.visual.load() != Visual::Cut) {
			filter.opacity = filter.fade_start * (1 - filter.elapsed / filter.out);
		}
	} else if (filter.stage == Stage::Swapping) {
		restore_visibility(filter);
		filter.stage = Stage::Showing;
		filter.elapsed = 0;
	} else if (filter.stage == Stage::Showing) {
		if (filter.in > 0 && filter.visual.load() != Visual::Cut)
			filter.opacity = std::min(1.0f, filter.elapsed / filter.in);
		const float hold = filter.preview_original ? 1.0f : 0.0f;
		if (filter.elapsed >= filter.in + hold) {
			stop_animation(filter);
			if (filter.preview_original) {
				write_text(parent.get(), filter.latest_caption.value_or(*filter.preview_original));
				filter.preview_original.reset();
			}
		}
	}
}

void render(void *opaque, gs_effect_t *)
{
	auto &filter = *static_cast<Filter *>(opaque);
	const float opacity = filter.opacity.load();
	if (!filter.effect || opacity >= 1.0f) {
		obs_source_skip_video_filter(filter.source);
		return;
	}
	if (!obs_source_process_filter_begin(filter.source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;
	gs_effect_set_float(gs_effect_get_param_by_name(filter.effect, "opacity"), opacity);
	gs_effect_set_int(gs_effect_get_param_by_name(filter.effect, "style"), static_cast<int>(filter.visual.load()));
	gs_effect_set_bool(gs_effect_get_param_by_name(filter.effect, "incoming"), filter.incoming.load());
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_end(filter.source, filter.effect, 0, 0);
	gs_blend_state_pop();
}

const char *name(void *)
{
	return obs_module_text("SlideshowCaption");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	try {
		auto filter = std::make_unique<Filter>(source);
		update(filter.get(), settings);
		char *path = obs_module_file("caption-fade.effect");
		obs_enter_graphics();
		if (path)
			filter->effect = gs_effect_create_from_file(path, nullptr);
		obs_leave_graphics();
		bfree(path);
		if (!filter->effect)
			blog(LOG_WARNING, "[slideshow-caption] Could not load the caption fade effect");
		return filter.release();
	} catch (const std::exception &error) {
		blog(LOG_ERROR, "[slideshow-caption] %s", error.what());
		return nullptr;
	}
}

void destroy(void *opaque)
{
	auto filter = std::unique_ptr<Filter>(static_cast<Filter *>(opaque));
	disconnect(*filter);
	restore_visibility(*filter);
	if (filter->preview_original) {
		auto parent = text_parent(*filter);
		write_text(parent.get(), *filter->preview_original);
	}
	obs_enter_graphics();
	if (filter->effect)
		gs_effect_destroy(filter->effect);
	obs_leave_graphics();
}

void remove_filter(void *opaque, obs_source_t *parent)
{
	if (!opaque)
		return;
	auto &filter = *static_cast<Filter *>(opaque);
	std::lock_guard lock_animation(filter.animation);
	if (filter.preview_original) {
		write_text(parent, *filter.preview_original);
		filter.preview_original.reset();
	}
	stop_animation(filter);
}

void defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "mode", static_cast<int>(CaptionMode::Filename));
	obs_data_set_default_string(settings, "filename_separator", " - ");
	obs_data_set_default_string(settings, "regex", "(?<title>.+)");
	obs_data_set_default_string(settings, "format", "{title}");
	obs_data_set_default_int(settings, duration_setting(Transition::Instant), 0);
	obs_data_set_default_int(settings, duration_setting(Transition::Fade), 500);
	obs_data_set_default_int(settings, duration_setting(Transition::MatchStyle), 500);
	obs_data_set_default_int(settings, duration_setting(Transition::Obs), 500);
}

bool property_visibility(obs_properties_t *properties, obs_property_t *, obs_data_t *settings)
{
	const auto mode = static_cast<CaptionMode>(obs_data_get_int(settings, "mode"));
	const auto part = static_cast<FilenamePart>(obs_data_get_int(settings, "filename_part"));
	const auto transition = static_cast<Transition>(obs_data_get_int(settings, "transition"));
	obs_property_set_visible(obs_properties_get(properties, "regex"), mode == CaptionMode::Regex);
	obs_property_set_visible(obs_properties_get(properties, "filename_part"), mode == CaptionMode::Filename);
	obs_property_set_visible(obs_properties_get(properties, "filename_separator"),
				 mode == CaptionMode::Filename && part != FilenamePart::Whole);
	obs_property_set_visible(obs_properties_get(properties, "format"),
				 mode == CaptionMode::Regex || mode == CaptionMode::Metadata);
	obs_property_set_visible(obs_properties_get(properties, "index"), mode == CaptionMode::TextIndex);
	const bool manual_duration = !obs_data_get_bool(settings, "match_duration");
	for (auto choice : {Transition::Instant, Transition::Fade, Transition::MatchStyle, Transition::Obs}) {
		obs_property_set_visible(obs_properties_get(properties, duration_setting(choice)),
					 manual_duration && choice == transition);
	}
	return true;
}

obs_properties_t *properties(void *opaque)
{
	auto *props = obs_properties_create();
	auto *slides = obs_properties_add_list(props, "slideshow", "Image Slideshow", OBS_COMBO_TYPE_LIST,
					     OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(slides, "Select a slideshow", "");
	obs_enum_sources([](void *data, obs_source_t *source) {
		if (is_slideshow(source))
			obs_property_list_add_string(static_cast<obs_property_t *>(data), obs_source_get_name(source),
						     obs_source_get_uuid(source));
		return true;
	}, slides);
	auto *mode = obs_properties_add_list(props, "mode", "Caption source", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, "Filename", static_cast<int>(CaptionMode::Filename));
	obs_property_list_add_int(mode, "Text file beside each image", static_cast<int>(CaptionMode::Sidecar));
	obs_property_list_add_int(mode, "Shared text file", static_cast<int>(CaptionMode::TextIndex));
	obs_property_list_add_int(mode, "Embedded metadata", static_cast<int>(CaptionMode::Metadata));
	obs_property_list_add_int(mode, "Filename regex", static_cast<int>(CaptionMode::Regex));
	obs_property_set_modified_callback(mode, property_visibility);
	auto *part = obs_properties_add_list(props, "filename_part", "Use", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(part, "Before separator", static_cast<int>(FilenamePart::Before));
	obs_property_list_add_int(part, "After separator", static_cast<int>(FilenamePart::After));
	obs_property_list_add_int(part, "Whole filename", static_cast<int>(FilenamePart::Whole));
	obs_property_set_modified_callback(part, property_visibility);
	obs_properties_add_text(props, "filename_separator", "Separator", OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "regex", "Filename regex", OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "format", "Caption template", OBS_TEXT_DEFAULT);
	obs_properties_add_path(props, "index", "Shared text file", OBS_PATH_FILE, "Text files (*.txt);;All files (*.*)",
				nullptr);

	auto *transition = obs_properties_add_list(props, "transition", "Caption transition", OBS_COMBO_TYPE_LIST,
						   OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(transition, "Instant", static_cast<int>(Transition::Instant));
	obs_property_list_add_int(transition, "Custom fade", static_cast<int>(Transition::Fade));
	obs_property_list_add_int(transition, "Match slideshow style", static_cast<int>(Transition::MatchStyle));
	obs_property_list_add_int(transition, "OBS show/hide", static_cast<int>(Transition::Obs));
	obs_property_set_modified_callback(transition, property_visibility);
	auto *duration = obs_properties_add_bool(props, "match_duration", "Match slideshow duration");
	obs_property_set_modified_callback(duration, property_visibility);
	for (auto choice : {Transition::Instant, Transition::Fade, Transition::MatchStyle, Transition::Obs})
		obs_properties_add_int(props, duration_setting(choice), "Duration (ms)", 0, 20000, 1);
	auto *preview = obs_properties_add_button2(props, "preview", "Preview on the Text source",
		[](obs_properties_t *, obs_property_t *, void *data) {
			if (data) {
				auto &filter = *static_cast<Filter *>(data);
				std::lock_guard lock(filter.mailbox);
				filter.preview_requested = true;
			}
			return false;
		}, opaque);
	obs_property_set_long_description(preview, "Temporarily updates the actual Text source, including live output.");
	return props;
}

} // namespace

extern "C" void register_slideshow_caption(void)
{
	obs_source_info info{};
	info.id = "slideshow_caption_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = name;
	info.create = create;
	info.destroy = destroy;
	info.get_defaults = defaults;
	info.get_properties = properties;
	info.update = update;
	info.video_tick = tick;
	info.video_render = render;
	info.filter_remove = remove_filter;
	obs_register_source(&info);
}
