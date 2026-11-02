#include "core/Snapshot.hpp"

#include <algorithm>
#include <set>

namespace klimt::core::snapshot {

std::vector<Document::Id> ids(const Document& d) {
    std::vector<Document::Id> out{kImageId, kSelectionId};   // the image first: a layer is put back on its canvas
    for (const auto& [id, l] : d.snapshot()) out.push_back(id);
    for (const auto& [id, c] : d.channels()) out.push_back(id);
    return out;
}

Snapshot take(const Document& d, const std::string& name) {
    Snapshot s;
    s.name = name;
    for (const auto& id : ids(d))
        if (auto v = d.get(id)) s.records.emplace(id, std::move(*v));
    return s;
}

std::string next_name(const std::vector<Snapshot>& have) {
    int n = 0;
    for (const auto& s : have)
        if (s.name.rfind("Snapshot ", 0) == 0) {
            try { n = std::max(n, std::stoi(s.name.substr(9))); } catch (...) {}
        }
    return "Snapshot " + std::to_string(n + 1);
}

bool restore(DocJournal& j, Document& d, const Snapshot& s) {
    // What the step touches: what is there now and what the snapshot holds
    // (the image first, then the rest).
    std::vector<Document::Id> all = ids(d);
    std::set<Document::Id> seen(all.begin(), all.end());
    for (const auto& [id, l] : s.records)
        if (seen.insert(id).second) all.push_back(id);
    return j.run(s.name, all, [&] {
        for (const auto& id : all) {
            const auto it = s.records.find(id);
            if (it != s.records.end()) d.put(id, it->second);
            else if (id != kImageId) d.put(id, std::nullopt);   // the image is never removed
        }
    });
}

}  // namespace klimt::core::snapshot
