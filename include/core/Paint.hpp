#pragma once
// core::Paint -- the Brush and the Eraser (s008 m4). GTK-free; docs/paint.md.
//
// "A modest brush -- painting is Krita's" (the tool note): round, size,
// hardness, opacity; the foreground colour; through the selection; the
// locks bite. One stroke = ONE step.
//
// THE MODEL. A stroke keeps a COVERAGE map (float, the tile grid): each dab
// raises coverage to max(coverage, dab), so a stroke never builds past its
// opacity where it crosses itself (Photoshop's opacity, not its flow). The
// layer during the stroke is always   original  (+)  colour at
// coverage x opacity x selection   -- recomputed from the ORIGINAL pixels,
// for the tiles the new dabs touched. So any moment of the stroke is exact,
// and the work is pure: a worker computes the next layer map from (original,
// current, coverage, dabs); the UI only lands it inside the stroke's gesture.
#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/Selection.hpp"

#include <optional>
#include <vector>

namespace klimt::core::paint {

struct Brush {
    double     size = 30;          // diameter, image pixels (1 .. 2500)
    double     hardness = 0.8;     // 0 soft .. 1 hard edge
    double     opacity = 1;        // 0 .. 1, the stroke's ceiling
    bool       erase = false;      // the Eraser: alpha down (no alpha: paints `background`)
    pixel::Px  colour;             // straight, the document's format (alpha ignored)
    pixel::Px  background;         // what the Eraser paints on a layer with no alpha
    bool       alpha_lock = false; // colour only, where there is something
    // s009 m3: a pen's PRESSURE (0..1; a mouse is always 1) may drive the
    // dab's size and / or its strength -- each a toggle in the strip.
    bool       pressure_size = true;
    bool       pressure_opacity = false;
    // s012 m1: FLOW -- how much each dab lays down. 1: a dab reaches its full
    // strength at once (coverage by max, as before). Below 1, dabs BUILD UP
    // where they overlap (Photoshop's flow), still capped by the opacity.
    double     flow = 1;
    double spacing() const { return std::max(0.5, size * 0.25); }
};

// A dab: where, and the pen's pressure there (1 for a mouse).
struct Dab { double x = 0, y = 0; float p = 1.f; };
// The light touch still leaves a mark: pressure never takes a dab below this
// fraction of the brush's size.
inline constexpr float kMinPressure = 0.05f;

// s012 m1: the STABILISER (Smoothing). A pulled string: the brush follows
// the pointer only once the pointer is more than `radius` (image px) away,
// staying `radius` behind it -- wobbles inside the string are ignored, curves
// come out smooth. finish() catches up to where the pointer is.
class Smoother {
public:
    Smoother() = default;
    explicit Smoother(double radius) : m_r(std::max(0.0, radius)) {}
    // The pointer is at p; where the brush is now (none when it didn't move).
    std::optional<struct Dab> push(const struct Dab& p);
    std::optional<struct Dab> finish();
    bool started() const { return m_started; }
private:
    double m_r = 0;
    bool   m_started = false;
    double m_x = 0, m_y = 0;
    struct { double x = 0, y = 0; float p = 1; } m_last;
};

// The dab centres from `from` to `to`, `spacing` apart, carrying the leftover
// distance in `carry` across calls (so a slow drag spaces dabs like a fast one).
// The first call of a stroke: carry = spacing, so it starts with a dab at `from`.
std::vector<select::Pt> dabs_along(select::Pt from, select::Pt to, double spacing, double& carry);
// s009 m3: the same with pressure, interpolated along the segment; the
// spacing follows the size the pressure gives (a light stroke spaces closer).
std::vector<Dab> dabs_along(const Dab& from, const Dab& to, const Brush& b, double& carry);
// A dab's radius and strength for this brush at pressure p.
double dab_radius(const Brush& b, float p);
float  dab_strength(const Brush& b, float p);

// The dab's strength at distance d from its centre (radius r = size / 2).
float dab_alpha(double d, const Brush& b);
float dab_alpha(double d, double r, double hardness);   // s009: at a given radius

// Coverage += the dabs (by max). `touched` (on the coverage's grid) gains
// every tile a dab reached. Coverage is F32 Gray, no alpha, at the image size.
pixel::TileMap add_dabs(const pixel::TileMap& cov, const Brush& b, const std::vector<select::Pt>& dabs, TileSet& touched);
pixel::TileMap add_pen_dabs(const pixel::TileMap& cov, const Brush& b, const std::vector<Dab>& dabs, TileSet& touched);
pixel::TileMap new_coverage(int w, int h);

// The layer as the stroke has it: `current` with each `touched` tile
// recomputed from `original`, `cov`, the selection `mask` and the brush.
pixel::TileMap apply(const pixel::TileMap& original, const pixel::TileMap& current, const pixel::TileMap& cov,
                     const pixel::TileMap* mask, const Brush& b, const TileSet& touched);

// May this layer be painted? "" yes, else why not (for a toast).
std::string refusal(const Document& d, const Document::Id& id, const Brush& b);

// A whole stroke, synchronously, as ONE step (the verb table; tests). The
// app does the same in pieces on workers (Shell_paint.cpp).
bool stroke(DocJournal& j, Document& d, const Document::Id& id, const std::vector<select::Pt>& path, Brush b);
// s009 m3: a stroke with the pen's pressure at each point.
bool stroke_pen(DocJournal& j, Document& d, const Document::Id& id, const std::vector<Dab>& path, Brush b);

}  // namespace klimt::core::paint
