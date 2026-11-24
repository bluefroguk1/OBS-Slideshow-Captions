#include "caption-content.h"

#include <exiv2/exiv2.hpp>
#include <pcre2.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using Fields = std::map<std::string, std::string>;

std::string metadata_token(std::string_view name)
{
	std::string token(name);
	for (auto &ch : token) {
		if (ch >= 'A' && ch <= 'Z')
			ch = static_cast<char>(ch - 'A' + 'a');
	}
	return token;
}

std::string format_caption(std::string_view pattern, const Fields &fields, bool metadata = false)
{
	std::string result;
	while (!pattern.empty()) {
		const auto open = pattern.find('{');
		const auto close = open == pattern.npos ? pattern.npos : pattern.find('}', open + 1);
		if (close == pattern.npos) {
			result += pattern;
			break;
		}
		result += pattern.substr(0, open);
		const auto name = pattern.substr(open + 1, close - open - 1);
		const auto field = fields.find(metadata ? metadata_token(name) : std::string(name));
		if (field != fields.end())
			result += field->second;
		pattern.remove_prefix(close + 1);
	}
	return result;
}

bool read_line(std::istream &input, std::string &line)
{
	if (!std::getline(input, line))
		return false;
	if (!line.empty() && line.back() == '\r')
		line.pop_back();
	return true;
}

void strip_bom(std::string &line)
{
	if (line.compare(0, 3, "\xEF\xBB\xBF") == 0)
		line.erase(0, 3);
}

std::string sidecar_caption(std::filesystem::path path)
{
	const auto filename = path.filename().u8string();
	std::ifstream input(path.replace_extension(".txt"), std::ios::binary);
	std::string line, result;
	if (!read_line(input, line))
		return {};
	strip_bom(line);
	// Older sidecars may include the image filename as a header.
	if (line != filename) {
		result = std::move(line);
		if (!input.eof())
			result += '\n';
	}
	while (read_line(input, line)) {
		result += line;
		if (!input.eof())
			result += '\n';
	}
	// Editors often end files with a newline; it would show as a blank line.
	while (!result.empty() && result.back() == '\n')
		result.pop_back();
	return result;
}

std::string indexed_caption(const std::filesystem::path &index, const std::filesystem::path &image)
{
	std::ifstream input(index, std::ios::binary);
	const auto relative = image.lexically_normal().lexically_relative(index.parent_path().lexically_normal());
	const auto path = relative.generic_u8string();
	const auto filename = image.filename().u8string();
	std::string line, path_match, name_match;
	bool first = true, found_path = false, found_name = false, duplicate_path = false, duplicate_name = false;
	while (read_line(input, line)) {
		if (first)
			strip_bom(line);
		first = false;
		const auto delimiter = line.find('\t');
		if (delimiter == line.npos)
			continue;
		const auto key = line.substr(0, delimiter);
		if (key == path && key != filename) {
			duplicate_path = found_path;
			found_path = true;
			path_match = line.substr(delimiter + 1);
		} else if (key == filename) {
			duplicate_name = found_name;
			found_name = true;
			name_match = line.substr(delimiter + 1);
		}
	}
	return found_path ? (duplicate_path ? "" : path_match) : (duplicate_name ? "" : name_match);
}

std::string regex_caption(const CaptionConfig &config, const std::string &stem)
{
	int error;
	PCRE2_SIZE offset;
	std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)> code(
		pcre2_compile(reinterpret_cast<PCRE2_SPTR>(config.pattern.data()), config.pattern.size(), PCRE2_UTF | PCRE2_UCP,
			      &error, &offset, nullptr),
		pcre2_code_free);
	if (!code)
		throw std::runtime_error("Invalid filename regex at byte " + std::to_string(offset));
	std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)> match(
		pcre2_match_data_create_from_pattern(code.get(), nullptr), pcre2_match_data_free);
	if (!match)
		throw std::bad_alloc();
	const int count = pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(stem.data()), stem.size(), 0, 0,
				      match.get(), nullptr);
	if (count == PCRE2_ERROR_NOMATCH)
		return {};
	if (count < 0)
		throw std::runtime_error("Filename regex could not be evaluated");
	const auto *positions = pcre2_get_ovector_pointer(match.get());
	const auto capture = [&](size_t group) {
		const auto start = positions[group * 2];
		return start == PCRE2_UNSET ? std::string{} : stem.substr(start, positions[group * 2 + 1] - start);
	};
	Fields fields;
	for (int group = 0; group < count; ++group)
		fields.emplace(std::to_string(group), capture(group));
	uint32_t names = 0, stride = 0;
	PCRE2_SPTR entry = nullptr;
	pcre2_pattern_info(code.get(), PCRE2_INFO_NAMECOUNT, &names);
	pcre2_pattern_info(code.get(), PCRE2_INFO_NAMEENTRYSIZE, &stride);
	pcre2_pattern_info(code.get(), PCRE2_INFO_NAMETABLE, &entry);
	for (uint32_t i = 0; i < names; ++i, entry += stride) {
		const auto group = (entry[0] << 8) | entry[1];
		if (group < count)
			fields.emplace(reinterpret_cast<const char *>(entry + 2), capture(group));
	}
	return format_caption(config.format, fields);
}

std::string metadata_value(Exiv2::Image &image, const std::string &tag)
{
	if (tag.compare(0, 5, "Exif.") == 0) {
		const auto &data = image.exifData();
		const auto value = data.findKey(Exiv2::ExifKey(tag));
		if (value == data.end())
			return {};
		if (tag.compare(0, 13, "Exif.Image.XP") == 0)
			return value->print(&data);
		const auto *comment = dynamic_cast<const Exiv2::CommentValue *>(&value->value());
		return comment ? comment->comment() : value->toString();
	}
	if (tag.compare(0, 5, "Iptc.") == 0) {
		const auto &data = image.iptcData();
		const auto value = data.findKey(Exiv2::IptcKey(tag));
		return value == data.end() ? std::string{} : value->toString();
	}
	if (tag.compare(0, 4, "Xmp.") == 0) {
		const auto &data = image.xmpData();
		const auto value = data.findKey(Exiv2::XmpKey(tag));
		if (value == data.end())
			return {};
		const auto *localized = dynamic_cast<const Exiv2::LangAltValue *>(&value->value());
		return localized ? localized->toString("x-default") : value->toString();
	}
	return {};
}

// Exiv2 takes a narrow path, which on Windows is only reliable for plain ASCII.
bool can_open_by_path(const std::filesystem::path &path)
{
#ifdef _WIN32
	const auto text = path.u8string();
	return std::all_of(text.begin(), text.end(), [](unsigned char ch) { return ch < 0x80; });
#else
	(void)path;
	return true;
#endif
}

std::string metadata_caption(const CaptionConfig &config, const std::filesystem::path &path)
{
	// Exiv2's XMP parser performs a process-wide lazy initialization.
	static std::mutex metadata_mutex;
	std::lock_guard lock(metadata_mutex);
	std::vector<Exiv2::byte> bytes;
	Exiv2::Image::UniquePtr image;
	if (can_open_by_path(path)) {
		// Let Exiv2 read only the parts of the file it needs.
		image = Exiv2::ImageFactory::open(path.string());
	} else {
		// Unicode Windows paths: read through the native API instead.
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		const auto size = input.tellg();
		if (size <= 0 || size > 64 * 1024 * 1024)
			return {};
		bytes.resize(static_cast<size_t>(size));
		input.seekg(0);
		if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size)))
			return {};
		image = Exiv2::ImageFactory::open(bytes.data(), bytes.size());
	}
	if (!image)
		return {};
	image->readMetadata();
	Fields fields;
	std::istringstream mapping(config.metadata);
	std::string line;
	while (read_line(mapping, line)) {
		const auto equals = line.find('=');
		if (equals == line.npos)
			continue;
		auto text = metadata_value(*image, line.substr(equals + 1));
		if (!text.empty())
			fields.emplace(metadata_token(std::string_view(line).substr(0, equals)), std::move(text));
	}
	for (const auto &field : metadata_fields) {
		if (fields.count(field.token))
			continue;
		for (const auto *tag : field.tags) {
			if (!tag)
				break;
			auto text = metadata_value(*image, tag);
			if (!text.empty()) {
				fields.emplace(field.token, std::move(text));
				break;
			}
		}
	}
	const auto authors = fields.find("authors");
	if (authors != fields.end())
		fields.emplace("artist", authors->second);
	return fields.empty() ? std::string{} : format_caption(config.format, fields, true);
}

} // namespace

std::string resolve_caption(const CaptionConfig &config, const std::filesystem::path &image)
{
	if (image.empty())
		return {};
	switch (config.mode) {
	case CaptionMode::Filename: {
		auto stem = image.stem().u8string();
		if (config.filename_part == FilenamePart::Whole || config.separator.empty())
			return stem;
		const auto separator = stem.find(config.separator);
		if (separator == std::string::npos)
			return stem;
		return config.filename_part == FilenamePart::After
			       ? stem.substr(separator + config.separator.size())
			       : stem.substr(0, separator);
	}
	case CaptionMode::Regex:
		return regex_caption(config, image.stem().u8string());
	case CaptionMode::Sidecar:
		return sidecar_caption(image);
	case CaptionMode::TextIndex:
		return indexed_caption(std::filesystem::u8path(config.index), image);
	case CaptionMode::Metadata:
		return metadata_caption(config, image);
	}
	return {};
}
