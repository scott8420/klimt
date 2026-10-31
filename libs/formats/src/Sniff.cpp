#include "formats/Formats.hpp"

#include <cctype>
#include <cstring>
#include <fstream>

namespace klimt::formats {

Kind sniff(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char b[256] = {};
    in.read(reinterpret_cast<char*>(b), sizeof b);
    const auto n = static_cast<std::size_t>(in.gcount());
    if (n >= 8 && std::memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return Kind::PNG;
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return Kind::JPEG;
    if (n >= 4 && ((b[0] == 'I' && b[1] == 'I' && (b[2] == 42 || b[2] == 43) && b[3] == 0) ||
                   (b[0] == 'M' && b[1] == 'M' && b[2] == 0 && (b[3] == 42 || b[3] == 43))))
        return Kind::TIFF;
    // s049: WebP is a RIFF container whose form type is "WEBP".
    // s050: Photoshop -- "8BPS", version 1 (PSD) or 2 (PSB; refused in words).
    if (n >= 6 && std::memcmp(b, "8BPS", 4) == 0 && b[4] == 0 && (b[5] == 1 || b[5] == 2)) return Kind::PSD;
    if (n >= 12 && std::memcmp(b, "RIFF", 4) == 0 && std::memcmp(b + 8, "WEBP", 4) == 0) return Kind::WebP;
    // A .klimt is a zip whose FIRST entry is "mimetype", stored, holding
    // "image/x-klimt" (OpenRaster's trick): the local header's 30 bytes, the
    // name, any extra field, then the data.
    if (n >= 30 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4) {
        const std::size_t name = b[26] | (b[27] << 8), extra = b[28] | (b[29] << 8);
        const std::size_t at = 30 + name + extra;
        if (name == 8 && std::memcmp(b + 30, "mimetype", 8) == 0 && at + 13 <= n &&
            std::memcmp(b + at, "image/x-klimt", 13) == 0)
            return Kind::Klimt;
        // s049: OpenRaster, the same trick with "image/openraster".
        if (name == 8 && std::memcmp(b + 30, "mimetype", 8) == 0 && at + 16 <= n &&
            std::memcmp(b + at, "image/openraster", 16) == 0)
            return Kind::ORA;
    }
    return Kind::Unknown;
}

const char* kind_name(Kind k) {
    switch (k) {
        case Kind::PNG:   return "PNG";
        case Kind::JPEG:  return "JPEG";
        case Kind::TIFF:  return "TIFF";
        case Kind::Klimt: return "Klimt";
        case Kind::WebP:  return "WebP";
        case Kind::ORA:   return "OpenRaster";
        case Kind::PSD:   return "Photoshop";
        case Kind::PDF:   return "PDF";
        case Kind::Unknown: break;
    }
    return "";
}

std::optional<Raster> read_image(const std::string& path, std::string& error, const Progress& p) {
    switch (sniff(path)) {
        case Kind::PNG:  return read_png(path, error, p);
        case Kind::JPEG: return read_jpeg(path, error, p);
        case Kind::TIFF: return read_tiff(path, error, p);
        case Kind::WebP: return read_webp(path, error, p);
        case Kind::Klimt: error = "a Klimt document, not an image"; return std::nullopt;
        case Kind::ORA:   error = "an OpenRaster document, not an image"; return std::nullopt;
        case Kind::PSD:   error = "a Photoshop document, not an image"; return std::nullopt;
        case Kind::PDF:   break;   // never sniffed
        case Kind::Unknown: break;
    }
    std::ifstream in(path, std::ios::binary);
    error = in ? "not a PNG, JPEG, TIFF or WebP file" : "can't be read";
    return std::nullopt;
}

// s051: the formats Export writes.
const std::vector<ExportFormat>& export_formats() {
    static const std::vector<ExportFormat> f{
        {Kind::PNG, "png", "PNG", ".png"},
        {Kind::JPEG, "jpeg", "JPEG", ".jpg"},
        {Kind::TIFF, "tiff", "TIFF", ".tif"},
        {Kind::WebP, "webp", "WebP", ".webp"},
        {Kind::ORA, "ora", "OpenRaster (.ora) -- layers, for Krita", ".ora"},
        {Kind::PSD, "psd", "Photoshop (.psd) -- layers", ".psd"},
        {Kind::PDF, "pdf", "PDF -- type kept as type", ".pdf"},
    };
    return f;
}
const ExportFormat* export_format(Kind k) {
    for (const auto& f : export_formats()) if (f.kind == k) return &f;
    return nullptr;
}
const ExportFormat* export_format(const std::string& key) {
    for (const auto& f : export_formats()) if (key == f.key) return &f;
    return nullptr;
}
namespace {
std::string lower_ext_of(const std::string& name) {
    const auto slash = name.find_last_of('/');
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot + 1 == name.size()) return {};
    std::string e = name.substr(dot);
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}
}  // namespace
const ExportFormat* export_format_of_name(const std::string& name) {
    std::string e = lower_ext_of(name);
    if (e == ".jpeg") e = ".jpg";
    if (e == ".tiff") e = ".tif";
    for (const auto& f : export_formats()) if (e == f.ext) return &f;
    return nullptr;
}
std::string with_extension(const std::string& name, Kind k) {
    const auto* f = export_format(k);
    if (!f) return name;
    if (export_format_of_name(name)) return name.substr(0, name.find_last_of('.')) + f->ext;
    return name + f->ext;
}

}  // namespace klimt::formats
