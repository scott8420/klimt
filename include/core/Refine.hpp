#pragma once
// core::refine -- Refine to Edges and the Refine brush (s038; Scott: "could it
// like pixelmator" ... "a crude pen boundary or selection could be the
// foundation"). docs/selection.md "Refine".
//
// A ROUGH AREA says "the figure is in here" -- any selection (the Lasso
// round it, a Marquee, the Wand's) or a path. Refine to Edges snaps it to
// the figure's real edge (core::isolate, the area in place of a path). The
// Refine brush then paints HINTS on top: a stroke adds (figure), an Alt
// stroke takes away (background); each re-runs the cut from the SAME rough
// area with every stroke so far, so a later stroke never undoes an earlier
// one's fix.
//
// The Session is that memory -- the rough area, the strokes, and which
// selection each run made -- kept by the Shell for the document in front.
// It never owns the selection: the journal does. A stroke checks where the
// selection is NOW: one this session made (Ctrl+Z may have gone back to an
// earlier one -- the strokes go back with it), or something else (a new
// Lasso, Deselect): a fresh session from it.
//
// GTK-free; the selftest walks it.
#include "core/Path.hpp"
#include "pixel/Pixel.hpp"

#include <cstddef>
#include <memory>
#include <vector>

namespace klimt::core::refine {

using Mask = std::shared_ptr<const pixel::TileMap>;

// A stroke onto the strokes so far. A stroke of the other kind that it
// crosses is OVERRULED -- taken off (Alt-painting over something just added
// means "no, not that": the add's sure pixels must not stand against it).
void add_stroke(path::Hints& h, const path::Stroke& s);

class Session {
public:
    // Refine to Edges: a fresh start from `rough` (nullptr: nothing selected)
    // while the selection is `now` (the rough area itself, or nothing when
    // the rough area is a path's) -- with `hints` already on it (a path
    // layer's strokes from s037).
    void start(Mask rough, Mask now, path::Hints hints = {});
    // A stroke painted while the selection is `now`. Returns true when it
    // carried on this session, false when it began a fresh one from `now`.
    bool stroke(const path::Stroke& s, const Mask& now);
    // A run landed: the selection it made (nullptr: nothing), from `hints`
    // (the strokes it was run with).
    void landed(Mask sel, path::Hints hints);
    void reset() { *this = {}; }

    bool active() const { return m_on; }
    const Mask& rough() const { return m_rough; }
    const path::Hints& hints() const { return m_hints; }
    std::size_t runs() const { return m_made.size(); }   // the start counts as one

private:
    struct Made { Mask sel; path::Hints hints; };
    bool m_on = false;
    Mask m_rough;
    path::Hints m_hints;
    std::vector<Made> m_made;   // [0]: the selection at the start, its strokes
};

}  // namespace klimt::core::refine
