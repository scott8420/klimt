#pragma once
// core::paths -- the PATH VERBS (s035), and the pixel side of paths: a
// path's coverage (-> a selection, a mask) and a selection's edge traced
// back into a path. docs/paths.md. The geometry is core/Path.hpp.
//
// Paths are PATH LAYERS (Kind::Path, Layer::path): every change is ONE step
// through the journal, the canvas verbs carry them, the .klimt keeps them.
#include "core/Document.hpp"
#include "core/Path.hpp"
#include "core/Selection.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace klimt::core::paths {

// ── pure ────────────────────────────────────────────────────────────────────
// The area the path encloses, anti-aliased, as a mask (the selection's
// format and the image's size); subpaths even-odd, open ones closed.
pixel::TileMap coverage(const path::Path& p, const pixel::Format& mf, int w, int h);
// The edge of a mask (coverage 1/2, between pixel centres -- marching
// squares) fitted with Beziers no further than `tolerance` px from it
// (Photoshop's Make Work Path: 0.5 .. 10, 2 by default). Specks smaller
// than a pixel are dropped. nullopt when cancelled.
std::optional<path::Path> traced(const pixel::TileMap& mask, double tolerance,
                                 const std::function<bool()>& cancelled = {});

// ── verbs (path layers themselves: layers::add_path_layer ...) ─────────────
// The path layer's outline; nullptr when `id` isn't a path layer.
std::shared_ptr<const path::Path> of(const Document& d, const Document::Id& id);
// Load as Selection (Ctrl+Enter): the path layer's area into the selection
// by `op`, feathered by `feather` px. Refused for an empty path.
bool to_selection(DocJournal& j, Document& d, const Document::Id& id, select::Op op = select::Op::Replace, double feather = 0);
// Make Path from Selection: the selection's edge, at `tolerance`, as a NEW
// path layer above `above`. Its id; "" refused (nothing selected).
Document::Id from_selection(DocJournal& j, Document& d, double tolerance = 2.0, const Document::Id& above = {});

// Isolate (s035 m7, core/Isolate.hpp): the path as a HINT -- the figure it
// goes round, found in what the eye sees (path masks off), as the selection
// by `op`. `band`: how far off the real edge the path may be. Refused for an
// empty path. Synchronous (the Shell runs isolate::isolated as a job and lands
// through select::set_if).
bool isolate(DocJournal& j, Document& d, const Document::Id& id, double band = 12, select::Op op = select::Op::Replace);
// What isolate reads: the composite with every path layer's mask off.
std::optional<pixel::TileMap> picture_for_isolate(const Image& im, const std::map<Document::Id, Layer>& layers);

// What a path layer's Mask switch masks (Scott: "it would run the whole group
// in my mind"): EVERY layer below it in its group (path layers skipped) --
// so at a group's top it masks the group's whole effect. Empty when it is the
// lowest in its group, or `path` isn't a path layer. Top first.
std::vector<Document::Id> masked_by(const std::map<Document::Id, Layer>& ls, const Document::Id& path);

// A closed rectangle / an ellipse (four smooth anchors) -- tests, examples.
path::Path rect_path(double x0, double y0, double x1, double y1);
path::Path ellipse_path(double cx, double cy, double rx, double ry);

}  // namespace klimt::core::paths
