#include "caption-content.h"

#include <obs-module.h>
#include <graphics/matrix4.h>
#include <util/bmem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
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
enum class Stage { Idle, PreviewStart, Hiding, Swapping, Showing, Capture, Blend };

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

struct SwipeLimits {
	float left = 0.0f;
	float right = 1.0f;
	bool bounded = false;
};

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
	// Tick selects the item. Render reads its current matrices after OBS lays out the text.
	std::shared_ptr<obs_sceneitem_t> swipe_item;
	std::atomic<bool> incoming{false};
	std::atomic<bool> capture_requested{false};
	std::atomic<bool> capture_ready{false};
	std::atomic<float> blend_progress{-1.0f};
	std::atomic<bool> swipe_text_box{false};
	Source slideshow_transition{nullptr, obs_source_release};
	gs_texrender_t *outgoing = nullptr;
	SwipeLimits outgoing_limits;
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

TextItems showing_text_items(obs_source_t *text)
{
	TextItems search{text, {}};
	obs_enum_scenes([](void *opaque, obs_source_t *source) {
		if (auto *scene = obs_scene_from_source(source); scene && obs_source_showing(source))
			obs_scene_enum_items(scene, collect_text_items, opaque);
		return true;
	}, &search);
	return search;
}

void select_swipe_item(Filter &filter, obs_source_t *parent)
{
	std::shared_ptr<obs_sceneitem_t> selected;
	auto *target = obs_filter_get_target(filter.source);
	if (target && obs_source_get_base_width(target) == obs_source_get_width(parent)) {
		auto search = showing_text_items(parent);
		if (search.items.size() == 1)
			selected = std::move(search.items.front());
	}
	std::atomic_store(&filter.swipe_item, std::move(selected));
}

SwipeLimits swipe_limits(Filter &filter)
{
	const auto item = std::atomic_load(&filter.swipe_item);
	if (!item || !obs_sceneitem_get_scene(item.get()) ||
	    obs_sceneitem_get_bounds_type(item.get()) == OBS_BOUNDS_NONE ||
	    obs_sceneitem_get_bounds_crop(item.get()))
		return {};
	obs_sceneitem_crop crop{};
	obs_sceneitem_get_crop(item.get(), &crop);
	if (crop.left || crop.right || crop.top || crop.bottom)
		return {};

	auto *target = obs_filter_get_target(filter.source);
	if (!target)
		return {};
	const auto width = obs_source_get_base_width(target);
	if (!width)
		return {};

	matrix4 draw, box;
	obs_sceneitem_get_draw_transform(item.get(), &draw);
	obs_sceneitem_get_box_transform(item.get(), &box);

	// Project the two box edges onto the texture's horizontal axis.
	const float extent = (draw.x.x * draw.x.x + draw.x.y * draw.x.y) * width;
	if (!(extent > 0.0f))
		return {};
	const float first = ((box.t.x - draw.t.x) * draw.x.x + (box.t.y - draw.t.y) * draw.x.y) / extent;
	const float span = (box.x.x * draw.x.x + box.x.y * draw.x.y) / extent;
	const float last = first + span;
	if (!std::isfinite(first) || !std::isfinite(last) || first == last)
		return {};
	return {std::min(first, last), std::max(first, last), true};
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
	std::atomic_store(&filter.swipe_item, std::shared_ptr<obs_sceneitem_t>{});
	filter.capture_requested = false;
	filter.blend_progress = -1.0f;
	filter.slideshow_transition.reset();
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
	if (transition == Transition::MatchStyle && visual != Visual::Cut && duration_ms && filter.outgoing) {
		select_swipe_item(filter, parent);
		filter.blend_progress = -1.0f;
		filter.capture_ready = false;
		filter.capture_requested = true;
		filter.in = duration_ms / 1000.0f;
		filter.stage = Stage::Capture;
		filter.slideshow_transition.reset();
		if (match_duration && !filter.preview_original) {
			obs_source_enum_active_sources(filter.slideshow.get(), [](obs_source_t *, obs_source_t *child, void *data) {
				auto &found = *static_cast<Source *>(data);
				if (!found && obs_source_get_type(child) == OBS_SOURCE_TYPE_TRANSITION)
					found.reset(obs_source_get_ref(child));
			}, &filter.slideshow_transition);
		}
		return;
	}
	if (transition == Transition::Obs && duration_ms > 0) {
		auto search = showing_text_items(parent);
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
	filter.swipe_text_box = obs_data_get_bool(settings, "swipe_text_box");
	const auto *duration_key = duration_setting(options.transition);
	options.duration_ms = static_cast<uint32_t>(std::clamp<int64_t>(obs_data_get_int(settings, duration_key), 0, 20000));
	std::lock_guard lock(filter.mailbox);
	const auto caption_fields = [](const CaptionConfig &caption) {
		return std::tie(caption.mode, caption.filename_part, caption.separator, caption.pattern, caption.format,
				caption.index, caption.metadata);
	};
	const bool caption_changed = caption_fields(filter.options.caption) != caption_fields(options.caption);
	const bool slideshow_changed = filter.options.slideshow != options.slideshow;
	const bool transition_changed = filter.options.transition != options.transition ||
					filter.options.match_duration != options.match_duration ||
					filter.options.duration_ms != options.duration_ms;
	filter.options = std::move(options);
	if (caption_changed && !slideshow_changed && filter.has_path)
		request_caption(filter);
	filter.options_changed |= caption_changed || slideshow_changed || transition_changed;
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
	} else if (filter.stage == Stage::Capture) {
		if (filter.capture_ready || filter.elapsed >= 0.1f) {
			filter.capture_requested = false;
			write_text(parent.get(), filter.pending_caption);
			filter.stage = Stage::Blend;
		}
	}
	if (filter.stage == Stage::Blend) {
		const float progress = filter.slideshow_transition
					       ? obs_transition_get_time(filter.slideshow_transition.get())
					       : filter.elapsed / filter.in;
		filter.blend_progress = filter.capture_ready ? std::clamp(progress, 0.0f, 1.0f) : -1.0f;
		select_swipe_item(filter, parent.get());
		const float hold = filter.preview_original ? 1.0f : 0.0f;
		if (progress >= 1.0f && filter.elapsed >= filter.in + hold) {
			stop_animation(filter);
			if (filter.preview_original) {
				write_text(parent.get(), filter.latest_caption.value_or(*filter.preview_original));
				filter.preview_original.reset();
			}
		}
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
	if (filter.swipe_text_box && filter.visual.load() == Visual::Swipe &&
	    (filter.stage == Stage::Hiding || filter.stage == Stage::Showing))
		select_swipe_item(filter, parent.get());
}

void set_draw_parameters(gs_effect_t *effect, Visual style, float opacity, bool incoming, SwipeLimits limits)
{
	gs_effect_set_int(gs_effect_get_param_by_name(effect, "style"), static_cast<int>(style));
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "opacity"), opacity);
	gs_effect_set_bool(gs_effect_get_param_by_name(effect, "incoming"), incoming);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "swipe_left"), limits.left);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "swipe_right"), limits.right);
}

void render_capture(Filter &filter, uint32_t width, uint32_t height)
{
	gs_texrender_reset(filter.outgoing);
	if (!width || !height || !gs_texrender_begin(filter.outgoing, width, height)) {
		obs_source_skip_video_filter(filter.source);
		return;
	}
	gs_projection_push();
	gs_matrix_push();
	gs_matrix_identity();
	gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	auto *effect = filter.effect;
	const bool captured = obs_source_process_filter_begin(filter.source, GS_RGBA, OBS_NO_DIRECT_RENDERING);
	if (captured) {
		set_draw_parameters(effect, Visual::Cut, 1.0f, false, {});
		obs_source_process_filter_end(filter.source, effect, 0, 0);
	}
	gs_blend_state_pop();
	gs_matrix_pop();
	gs_projection_pop();
	gs_texrender_end(filter.outgoing);
	if (!captured) {
		obs_source_skip_video_filter(filter.source);
		return;
	}
	auto *texture = gs_texrender_get_texture(filter.outgoing);
	filter.outgoing_limits = swipe_limits(filter);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	const bool srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	set_draw_parameters(effect, Visual::Cut, 1.0f, false, {});
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(texture, 0, 0, 0);
	gs_enable_framebuffer_srgb(srgb);
	gs_blend_state_pop();
	filter.capture_ready = true;
}

bool render_blend(Filter &filter, float progress, uint32_t width, uint32_t height)
{
	auto *texture = gs_texrender_get_texture(filter.outgoing);
	if (!texture || !width)
		return false;
	if (!obs_source_process_filter_begin(filter.source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return true;
	const auto style = filter.visual.load();
	const auto limits = swipe_limits(filter);
	const auto old = filter.outgoing_limits;
	const bool bounded = limits.bounded && old.bounded;
	const auto span = limits.right - limits.left;
	const auto old_span = old.right - old.left;
	const float fit = static_cast<float>(width) / gs_texture_get_width(texture);
	const float ratio = bounded ? old_span / span : fit;
	const float offset = bounded ? old.left - limits.left * ratio : (1.0f - fit) / 2.0f;
	const float text_left = std::min(0.0f, -offset / ratio);
	const float text_right = std::max(1.0f, (1.0f - offset) / ratio);
	const bool use_box = bounded && (style == Visual::Slide || (style == Visual::Swipe && filter.swipe_text_box));
	const float motion_left = use_box ? limits.left : (style == Visual::Swipe ? text_left : 0.0f);
	const float motion_right = use_box ? limits.right : (style == Visual::Swipe ? text_right : 1.0f);
	const float left = std::min(text_left, motion_left);
	const float right = std::max(text_right, motion_right);
	auto *effect = filter.effect;
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "outgoing_image"), texture);
	gs_effect_set_int(gs_effect_get_param_by_name(effect, "style"), static_cast<int>(style));
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "progress"), progress);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "old_ratio"), ratio);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "old_offset"), offset);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "old_height_ratio"),
			    static_cast<float>(height) / gs_texture_get_height(texture));
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "canvas_left"), left);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "canvas_span"), right - left);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "swipe_left"), motion_left);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "swipe_right"), motion_right);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_process_filter_tech_end(filter.source, effect, 0, 0, "Blend");
	gs_blend_state_pop();
	return true;
}

void render(void *opaque, gs_effect_t *)
{
	auto &filter = *static_cast<Filter *>(opaque);
	const auto progress = filter.blend_progress.load();
	if (filter.effect && filter.outgoing && (filter.capture_requested || (progress >= 0.0f && progress < 1.0f))) {
		auto *target = obs_filter_get_target(filter.source);
		if (target) {
			const auto width = obs_source_get_base_width(target);
			if (filter.capture_requested && !filter.capture_ready) {
				render_capture(filter, width, obs_source_get_base_height(target));
				return;
			}
			if (progress >= 0.0f && render_blend(filter, progress, width, obs_source_get_base_height(target)))
				return;
		}
	}
	const float opacity = filter.opacity.load();
	if (!filter.effect || opacity >= 1.0f) {
		obs_source_skip_video_filter(filter.source);
		return;
	}
	if (!obs_source_process_filter_begin(filter.source, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;
	const auto style = filter.visual.load();
	set_draw_parameters(filter.effect, style, opacity, filter.incoming.load(),
			    style == Visual::Swipe ? swipe_limits(filter) : SwipeLimits{});
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
		if (filter->effect)
			filter->outgoing = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
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
	if (filter->outgoing)
		gs_texrender_destroy(filter->outgoing);
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
	obs_property_set_visible(obs_properties_get(properties, "swipe_text_box"), transition == Transition::MatchStyle);
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
	obs_properties_add_bool(props, "swipe_text_box", "Use text box boundary for swipe");
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
