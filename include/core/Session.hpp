#pragma once
// core/Session -- THE OPEN TABS, REMEMBERED (s023). On quit the window writes
// the files its tabs came from (a .klimt, or the image a tab was opened
// from) and which one was in front; Klimt started with no file opens them
// again. Unsaved changes are not kept (quitting asked about them); a tab
// never saved and not from a file (New, the demo) is not remembered.
// GTK-free; the UI hands in the path (~/.config/klimt/session.json).
#include <string>
#include <vector>

namespace klimt::core {

struct Session {
    std::vector<std::string> files;   // the tabs, in order
    int active = 0;                   // the one in front (an index into files)
    bool operator==(const Session&) const = default;
};

Session load_session(const std::string& file);   // missing / broken: empty
bool    save_session(const std::string& file, const Session& s);
// The files that still exist (and the front one's new index, -1 if it is gone).
Session existing(const Session& s);

}  // namespace klimt::core
