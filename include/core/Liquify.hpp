#pragma once
// core::liquify -- Filter > Liquify (s076). GTK-free; docs/liquify.md.
//
// A DISPLACEMENT FIELD over the canvas: at each node (every `step` px) the
// offset to where that place looks for its pixel -- the picture at p is the
// source at p + D(p) (D bilinear between nodes). The brushes reshape D:
//   Forward Warp -- push pixels along the stroke;
//   Reconstruct  -- ease D back toward nothing (the original);
//   Twirl        -- turn them about the brush (Alt: the other way);
//   Pucker / Bloat -- draw them in toward / push them out from the brush.
// Each dab is: for the nodes under the brush, n' = where the brush says n
// now looks, and D(n) = D(n') + (n' - n) -- so dabs compose (a push on a
// push, a twirl of a twirl) without the picture tearing.
// The field is the UI thread's (dabs are cheap: the nodes under the brush);
// applying it to pixels is the shared resample (xform::over_mapped), a
// worker's job; the result lands as ONE step.
#include "core/Document.hpp"
#include "core/Selection.hpp"
#include "core/Transform.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::liquify {

using Pt = select::Pt;

enum class Tool { Forward, Reconstruct, Twirl, Pucker, Bloat };
constexpr int kTools = 5;
const char* tool_name(Tool t);

struct Brush {
    Tool   tool = Tool::Forward;
    double radius = 50;       // image px
    double pressure = 0.5;    // 0..1: how much a dab does (the pen's pressure times the strip's)
    double rate = 0.5;        // 0..1: Twirl / Pucker / Bloat / Reconstruct per dab
};

class Field {
public:
    Field() = default;
    Field(int width, int height, int step = 4);
    int width() const { return m_w; }
    int height() const { return m_h; }
    int step() const { return m_step; }
    bool valid() const { return m_gw > 0; }
    // D at an image point (bilinear; 0 beyond the field).
    Pt at(double x, double y) const;
    // Every node 0 (within a hair).
    bool identity() const;
    // The pixels any non-zero node can move (image coords, with the brush's
    // reach); empty: none.
    pixel::Rect touched() const { return m_touched; }
    // One dab at c. `motion`: Forward Warp's push since the last dab (image
    // px); `reverse`: Twirl the other way (Alt), Pucker <-> Bloat.
    void dab(const Brush& b, Pt c, Pt motion = {0, 0}, bool reverse = false);
    void clear();

private:
    int m_w = 0, m_h = 0, m_step = 4, m_gw = 0, m_gh = 0;
    std::vector<float> m_dx, m_dy;
    pixel::Rect m_touched;
    void touch(const pixel::Rect& r);
};

// The layer's pixels through the field, over `field.touched()` (the rest
// shared as it is). nullopt: cancelled. Quality::Fast while brushing,
// Best (bicubic) for the step that lands.
std::optional<pixel::TileMap> apply(const pixel::TileMap& src, const Field& f, xform::Quality q,
                                    const std::function<bool()>& cancelled = {});

// "" yes, else why not: a pixel layer with pixels, not locked (Pixels / All),
// not type.
std::string refusal(const Document& d, const Document::Id& id);
// The verb: the layer through the field, one step "Liquify". False: refused,
// or the field changes nothing.
bool liquify(DocJournal& j, Document& d, const Document::Id& id, const Field& f);

}  // namespace klimt::core::liquify
