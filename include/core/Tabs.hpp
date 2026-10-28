#pragma once
// core::Tabs -- DOCUMENT TABS (s020). Several images in one window. GTK-free;
// the Shell's tab bar sits on it. docs/tabs.md.
//
// ONE document and ONE journal are live -- the ones every panel, verb and job
// already holds by reference. The other tabs are PARKED: their records
// (Document::Parked) and their history (Journal::History) moved out whole.
// show(i) swaps the live pair with tab i's: O(1) (maps of shared pixels), no
// step, nothing copied. So the rest of the app never learns there are tabs.
//
//   * each tab keeps its OWN undo history and its own saved mark;
//   * serials are one counter for the window, so a tab's saved mark can never
//     match another tab's state;
//   * a switch is refused inside the door or a gesture (a stroke in flight);
//     the Shell refuses it too while a job on the document is running.
//
// A tab's `key` never changes and is never reused: a job's landing that names
// a tab (a save) finds it by key, wherever it is now and whether or not it is
// the live one.
#include "core/Document.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core {

class Tabs {
public:
    struct Info {
        std::uint64_t key = 0;
        std::string   path;            // the .klimt it saves to; "" = never saved
        std::string   source;          // s023: the file it was opened from (an image or a .klimt); "" = none
        std::string   name = "No Image";
        std::uint64_t saved_mark = 0;  // its journal's mark when it last matched a file
    };

    // Tab 0 is the live document as it stands.
    Tabs(Document& d, DocJournal& j) : m_doc(d), m_journal(j) {
        Slot s;
        s.info.key = ++m_keys;
        s.info.saved_mark = j.mark();
        m_slots.push_back(std::move(s));
    }

    std::size_t count() const { return m_slots.size(); }
    std::size_t live() const { return m_live; }
    Info&       info(std::size_t i) { return m_slots.at(i).info; }
    const Info& info(std::size_t i) const { return m_slots.at(i).info; }
    Info&       live_info() { return m_slots[m_live].info; }

    std::optional<std::size_t> by_key(std::uint64_t key) const {
        for (std::size_t i = 0; i < m_slots.size(); ++i) if (m_slots[i].info.key == key) return i;
        return std::nullopt;
    }
    std::optional<std::size_t> by_path(const std::string& path) const {
        if (path.empty()) return std::nullopt;
        for (std::size_t i = 0; i < m_slots.size(); ++i) if (m_slots[i].info.path == path) return i;
        return std::nullopt;
    }

    // The mark tab i's history stands at (live: the journal's own).
    std::uint64_t mark(std::size_t i) const {
        if (i == m_live) return m_journal.mark();
        const auto& h = m_slots.at(i).hist;
        return h.at > 0 ? h.steps[h.at - 1].serial : h.base;
    }
    bool dirty(std::size_t i) const { return mark(i) != m_slots.at(i).info.saved_mark; }
    // Tab i's image size (live: the document's).
    const Image& image(std::size_t i) const { return i == m_live ? m_doc.image() : m_slots.at(i).doc.image; }
    std::size_t layer_count(std::size_t i) const { return i == m_live ? m_doc.count() : m_slots.at(i).doc.items.size(); }

    // A new, EMPTY tab after `after` (no image, a fresh history), parked; its
    // index. Not shown: show() it.
    std::size_t add(std::size_t after) {
        Slot s;
        s.info.key = ++m_keys;
        s.hist = m_journal.fresh_history();
        s.info.saved_mark = s.hist.base;
        const std::size_t at = std::min(after + 1, m_slots.size());
        m_slots.insert(m_slots.begin() + static_cast<std::ptrdiff_t>(at), std::move(s));
        if (m_live >= at) ++m_live;
        return at;
    }

    // Tab i becomes the live one. False (nothing changed) when the journal
    // refuses: inside the door or a gesture.
    bool show(std::size_t i) {
        if (i >= m_slots.size()) return false;
        if (i == m_live) return true;
        if (!m_journal.swap_history(m_slots[m_live].hist)) return false;
        {
            Document::Loading load(m_doc);
            m_doc.swap_parked(m_slots[m_live].doc);    // the live one goes out
        }
        // Now the slot of the old live tab holds it; bring i in.
        m_journal.swap_history(m_slots[i].hist);
        {
            Document::Loading load(m_doc);
            m_doc.swap_parked(m_slots[i].doc);
        }
        m_live = i;
        return true;
    }

    // Close tab i. The last tab is never removed: it becomes empty (no image,
    // a fresh history) -- the window always has a document. Closing the live
    // tab shows its neighbour first (the one to the right, else left). False
    // when the switch is refused.
    bool close(std::size_t i) {
        if (i >= m_slots.size()) return false;
        if (m_slots.size() == 1) {
            if (m_journal.open()) return false;
            Slot fresh;
            fresh.info.key = ++m_keys;
            fresh.hist = m_journal.fresh_history();
            fresh.info.saved_mark = fresh.hist.base;
            if (!m_journal.swap_history(fresh.hist)) return false;
            {
                Document::Loading load(m_doc);
                m_doc.swap_parked(fresh.doc);
            }
            m_slots[0].info = fresh.info;
            return true;
        }
        if (i == m_live && !show(i + 1 < m_slots.size() ? i + 1 : i - 1)) return false;
        m_slots.erase(m_slots.begin() + static_cast<std::ptrdiff_t>(i));
        if (m_live > i) --m_live;
        return true;
    }

    // Move tab `from` to index `to` (the tab bar's drag). The live one stays live.
    void reorder(std::size_t from, std::size_t to) {
        if (from >= m_slots.size() || to >= m_slots.size() || from == to) return;
        const auto key = m_slots[m_live].info.key;
        Slot s = std::move(m_slots[from]);
        m_slots.erase(m_slots.begin() + static_cast<std::ptrdiff_t>(from));
        m_slots.insert(m_slots.begin() + static_cast<std::ptrdiff_t>(to), std::move(s));
        m_live = *by_key(key);
    }

private:
    struct Slot {
        Info                info;
        Document::Parked    doc;    // empty while the tab is the live one
        DocJournal::History hist;   // likewise
    };
    Document&          m_doc;
    DocJournal&        m_journal;
    std::vector<Slot>  m_slots;
    std::size_t        m_live = 0;
    std::uint64_t      m_keys = 0;
};

}  // namespace klimt::core
