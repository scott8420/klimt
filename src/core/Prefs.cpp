// core/Prefs -- the app's settings pump (s014 m4). See core/Prefs.hpp.
#include "core/Prefs.hpp"

#include "json.hpp"
#include "tint/Tint.hpp"   // s070: intent keys

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace klimt::core {

namespace {
nlohmann::json read(const std::string& file) {
    std::ifstream f(file);
    if (!f) return nlohmann::json::object();
    try {
        nlohmann::json j;
        f >> j;
        return j.is_object() ? j : nlohmann::json::object();
    } catch (const std::exception&) {
        return nlohmann::json::object();
    }
}
}  // namespace

Prefs load_prefs(const std::string& file) {
    Prefs p;
    const auto j = read(file);
    if (j.contains("dock_split") && j["dock_split"].is_number_integer())
        p.dock_split = std::clamp(j["dock_split"].get<int>(), 120, 4000);
    if (j.contains("rulers") && j["rulers"].is_boolean()) p.rulers = j["rulers"].get<bool>();   // s031
    if (j.contains("ruler_unit") && j["ruler_unit"].is_string()) p.ruler_unit = j["ruler_unit"].get<std::string>();
    for (const auto& [k, f] : {std::pair<const char*, bool Prefs::*>{"show_guides", &Prefs::show_guides},
                               {"lock_guides", &Prefs::lock_guides}, {"snap", &Prefs::snap}})   // s032
        if (j.contains(k) && j[k].is_boolean()) p.*f = j[k].get<bool>();
    if (j.contains("path_tolerance") && j["path_tolerance"].is_number())   // s035
        p.path_tolerance = std::clamp(j["path_tolerance"].get<double>(), 0.5, 10.0);
    if (j.contains("export") && j["export"].is_object()) {   // s050
        const auto& e = j["export"];
        auto& o = p.export_options;
        const auto num = [&](const char* k, int& v, int lo, int hi) {
            if (e.contains(k) && e[k].is_number_integer()) v = std::clamp(e[k].get<int>(), lo, hi);
        };
        const auto flag = [&](const char* k, bool& v) { if (e.contains(k) && e[k].is_boolean()) v = e[k].get<bool>(); };
        num("jpeg_quality", o.jpeg_quality, 1, 100);
        num("webp_quality", o.webp_quality, 0, 100);
        num("png_level", o.png_level, 0, 9);
        flag("webp_lossless", o.webp_lossless);
        flag("tiff_deflate", o.tiff_deflate);
    }
    if (j.contains("export_format") && j["export_format"].is_string() && formats::export_format(j["export_format"].get<std::string>()))
        p.export_format = j["export_format"].get<std::string>();   // s051
    if (j.contains("proof_profile") && j["proof_profile"].is_string()) p.proof_profile = j["proof_profile"].get<std::string>();   // s069
    if (j.contains("proof_paper") && j["proof_paper"].is_boolean()) p.proof_paper = j["proof_paper"].get<bool>();
    {   // s070
        const auto str = [&](const char* k, std::string& v) { if (j.contains(k) && j[k].is_string()) v = j[k].get<std::string>(); };
        const auto flag = [&](const char* k, bool& v) { if (j.contains(k) && j[k].is_boolean()) v = j[k].get<bool>(); };
        const auto intent = [&](const char* k, std::string& v) {
            klimt::tint::Intent i;
            if (j.contains(k) && j[k].is_string() && klimt::tint::intent_from(j[k].get<std::string>(), i)) v = j[k].get<std::string>();
        };
        str("working_cmyk", p.working_cmyk);
        str("working_gray", p.working_gray);
        intent("intent", p.intent);
        intent("proof_intent", p.proof_intent);
        flag("bpc", p.bpc);
        flag("proof_bpc", p.proof_bpc);
    }
    return p;
}

bool save_prefs(const std::string& file, const Prefs& p) {
    try {
        fs::create_directories(fs::path(file).parent_path());
    } catch (const std::exception&) {
        return false;
    }
    auto j = read(file);   // keep what we don't know
    j["dock_split"] = p.dock_split;
    j["rulers"] = p.rulers;            // s031
    j["ruler_unit"] = p.ruler_unit;
    j["show_guides"] = p.show_guides;   // s032
    j["lock_guides"] = p.lock_guides;
    j["snap"] = p.snap;
    j["path_tolerance"] = p.path_tolerance;   // s035
    const auto& o = p.export_options;          // s050
    j["export"] = {{"jpeg_quality", o.jpeg_quality}, {"webp_quality", o.webp_quality}, {"webp_lossless", o.webp_lossless},
                   {"png_level", o.png_level}, {"tiff_deflate", o.tiff_deflate}};
    j["export_format"] = p.export_format;   // s051
    j["proof_profile"] = p.proof_profile;   // s069
    j["proof_paper"] = p.proof_paper;
    j["proof_intent"] = p.proof_intent;   // s070
    j["proof_bpc"] = p.proof_bpc;
    j["working_cmyk"] = p.working_cmyk;
    j["working_gray"] = p.working_gray;
    j["intent"] = p.intent;
    j["bpc"] = p.bpc;
    std::ofstream f(file);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return static_cast<bool>(f);
}

}  // namespace klimt::core
