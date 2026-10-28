// klimt_selftest -- THE OPEN TABS, REMEMBERED (s023).
#include "core/Session.hpp"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unistd.h>

namespace core = klimt::core;
using Check = std::function<void(const std::string&, bool, const std::string&)>;

void selftest_session(const Check& check) {
    std::cout << "\n-- the open tabs, remembered (s023) --\n";
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("klimt_session_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string file = (dir / "session.json").string();
    check("session: none yet -- empty", core::load_session(file).files.empty(), "");
    const std::string a = (dir / "a.png").string(), b = (dir / "b.klimt").string(), gone = (dir / "gone.jpg").string();
    std::ofstream(a) << "x";
    std::ofstream(b) << "x";
    core::Session s{{a, gone, b}, 2};
    check("session: written, read back", core::save_session(file, s) && core::load_session(file) == s, "");
    const auto e = core::existing(s);
    check("existing: the missing file dropped, the front one re-indexed", e.files.size() == 2 && e.files[1] == b && e.active == 1,
          std::to_string(e.active));
    check("existing: the front one gone -> -1", core::existing(core::Session{{a, gone}, 1}).active == -1, "");
    { std::ofstream(file) << "nonsense"; }
    check("session: garbage -> empty", core::load_session(file).files.empty(), "");
    { std::ofstream(file) << R"({"files":["/x", 3, ""], "active": 9})"; }
    const auto g = core::load_session(file);
    check("session: bad entries dropped, a bad index -> 0", g.files.size() == 1 && g.active == 0, "");
    fs::remove_all(dir);
}
