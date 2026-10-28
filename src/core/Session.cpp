// core/Session -- the open tabs, remembered (s023). See core/Session.hpp.
#include "core/Session.hpp"

#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace klimt::core {

Session load_session(const std::string& file) {
    Session s;
    std::ifstream f(file);
    if (!f) return s;
    nlohmann::json j;
    try { f >> j; } catch (const std::exception&) { return s; }
    if (!j.is_object() || !j.contains("files") || !j["files"].is_array()) return s;
    for (const auto& e : j["files"])
        if (e.is_string() && !e.get<std::string>().empty()) s.files.push_back(e.get<std::string>());
    s.active = j.contains("active") && j["active"].is_number_integer() ? j["active"].get<int>() : 0;
    if (s.active < 0 || s.active >= static_cast<int>(s.files.size())) s.active = 0;
    return s;
}

bool save_session(const std::string& file, const Session& s) {
    try { fs::create_directories(fs::path(file).parent_path()); } catch (const std::exception&) { return false; }
    nlohmann::json j;
    j["version"] = 1;
    j["files"] = s.files;
    j["active"] = s.active;
    const std::string tmp = file + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << j.dump(2) << "\n";
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    return !ec;
}

Session existing(const Session& s) {
    Session out;
    out.active = -1;
    for (std::size_t i = 0; i < s.files.size(); ++i) {
        std::error_code ec;
        if (!fs::is_regular_file(s.files[i], ec)) continue;
        if (static_cast<int>(i) == s.active) out.active = static_cast<int>(out.files.size());
        out.files.push_back(s.files[i]);
    }
    return out;
}

}  // namespace klimt::core
