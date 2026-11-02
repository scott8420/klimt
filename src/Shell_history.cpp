#include "Shell.hpp"

// Shell_history.cpp -- HISTORY's snapshots (s077; REQUIREMENTS added 7).
// docs/undo.md "Snapshots". A snapshot is the whole document by value
// (core::snapshot); going back to one is one step through the journal. The
// snapshots are view state, kept per document tab, gone with its picture.

namespace klimt {

// ── s077: History's snapshots ────────────────────────────────────────────────
void Shell::on_snapshot(HistoryPanel::Snap what, std::size_t i) {
    if (!has_image()) return;
    auto& v = live_snapshots();
    if (what == HistoryPanel::Snap::Take) {
        if (m_liq.active || m_xform.active || m_stroke.active) { toast("Snapshot: finish what's under way first (Enter or Esc)"); return; }
        v.push_back(core::snapshot::take(m_doc, core::snapshot::next_name(v)));
        toast("Snapshot: “" + v.back().name + "” taken");
    } else if (what == HistoryPanel::Snap::Restore && i < v.size()) {
        if (m_liq.active || m_xform.active || m_stroke.active) { toast("Snapshot: finish what's under way first (Enter or Esc)"); return; }
        if (!core::snapshot::restore(m_journal, m_doc, v[i])) toast("Snapshot: the document is already as “" + v[i].name + "” holds it");
        else {   // what was picked and is still there; else the top layer
            std::vector<core::Document::Id> sel;
            for (const auto& id : m_layers.selection()) if (m_doc.get(id)) sel.push_back(id);
            if (sel.empty()) { const auto top = m_doc.panel(""); if (!top.empty()) sel.push_back(top.front()); }
            m_layers.select(sel, false);
            queue_ants();
            update_channels();
        }
    } else if (what == HistoryPanel::Snap::Delete && i < v.size()) {
        toast("Snapshot: “" + v[i].name + "” deleted");
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
    }
    update_snapshots();
}

void Shell::update_snapshots() {
    std::vector<std::string> names;
    for (const auto& s : live_snapshots()) names.push_back(s.name);
    m_history.set_snapshots(names);
}

}  // namespace klimt
