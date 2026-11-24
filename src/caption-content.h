#pragma once

#include <array>
#include <filesystem>
#include <string>

enum class CaptionMode { Regex, Sidecar, TextIndex, Metadata, Filename };
enum class FilenamePart { Before, After, Whole };

struct CaptionConfig {
	CaptionMode mode = CaptionMode::Filename;
	FilenamePart filename_part = FilenamePart::Before;
	std::string separator;
	std::string pattern;
	std::string format;
	std::string index;
	std::string metadata;
};

struct MetadataField {
	const char *token;
	std::array<const char *, 4> tags;
};

inline constexpr MetadataField metadata_fields[] = {
	{"title", {"Exif.Image.XPTitle", "Xmp.dc.title", "Iptc.Application2.ObjectName", "Exif.Image.ImageDescription"}},
	{"subject", {"Exif.Image.XPSubject", "Xmp.photoshop.Headline", "Iptc.Application2.Headline"}},
	{"tags", {"Exif.Image.XPKeywords", "Xmp.dc.subject", "Iptc.Application2.Keywords"}},
	{"comments", {"Exif.Image.XPComment", "Exif.Photo.UserComment", "Xmp.dc.description", "Iptc.Application2.Caption"}},
	{"authors", {"Exif.Image.XPAuthor", "Exif.Image.Artist", "Xmp.dc.creator", "Iptc.Application2.Byline"}},
};

std::string resolve_caption(const CaptionConfig &config, const std::filesystem::path &image);
