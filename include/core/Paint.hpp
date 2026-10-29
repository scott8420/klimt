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

// s017: the brush TIP's shape. Round (the s008 dab); Square (a soft-cornered
// square); Chalk (round, broken by a paper grain fixed to the IMAGE, so
// strokes over each other share the grain, as chalk on paper does).
enum class Tip : std::uint8_t { Round, Square, Chalk };

// s028: brush DYNAMICS (Photoshop's Shape Dynamics, Scattering, Color
// Dynamics). All off by default -- a brush without them paints exactly as
// before. The randomness is drawn when the dabs are MADE (paint::Jitter, the
// UI thread), so the pixel work stays pure and a stroke can be replayed.
struct Dynamics {
    // Shape: each dab's size drops by up to size_jitter (0..1) of the brush's,
    // never below min_size (0..1); turns by up to +-180 x angle_jitter; flattens
    // by up to roundness_jitter, never below min_roundness (0..1) of the tip's.
    double size_jitter = 0, min_size = 0;
    double angle_jitter = 0;
    double roundness_jitter = 0, min_roundness = 0.25;
    // Scattering: each dab lands up to scatter x the diameter (0..10, Photoshop's
    // 0..1000 %) off the path, across it -- or along it too (both_axes). COUNT
    // (1..16) dabs at each spacing step, fewer by up to count_jitter (0..1).
    double scatter = 0;
    bool   both_axes = false;
    int    count = 1;
    double count_jitter = 0;
    // Colour (s028 m2): each dab's colour moves from the foreground toward the
    // background by up to fg_bg; its hue by up to +-180 x hue, its saturation and
    // brightness by up to +-sat / +-bri. per_tip false: one draw per stroke.
    double fg_bg = 0, hue = 0, sat = 0, bri = 0;
    bool   per_tip = true;
    bool shape() const { return size_jitter > 0 || angle_jitter > 0 || roundness_jitter > 0; }
    bool scattering() const { return scatter > 0 || count > 1; }
    bool colour() const { return fg_bg > 0 || hue > 0 || sat > 0 || bri > 0; }
    bool any() const { return shape() || scattering() || colour(); }
};

// s033: the brush's TEXTURE (Photoshop's): a pattern pressed into the
// stroke, fixed to the IMAGE like paper grain -- its light parts take the
// paint, its dark parts less. `depth` 0..1; Multiply scales a dab's strength
// by the grain, Subtract takes the grain's darkness away from it. A built-in
// pattern (index) or yours (`image`, straight RGB + alpha).
struct Texture {
    bool   on = false;
    int    builtin = 0;
    std::shared_ptr<const pixel::TileMap> image;
    double scale = 1;        // 0.1 .. 10
    double depth = 0.5;      // 0 .. 1
    bool   subtract = false;
    bool   invert = false;
    // The grain's strength factor at image pixel (x, y): 0 .. 1.
    float at(int x, int y, float a) const;
};

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
    // s015: CLONE STAMP -- the colour (and its alpha) comes from `source` at
    // (x + dx, y + dy), rounded to whole pixels, instead of `colour`. The
    // source is the layer's own pixels as the stroke began, or what the eye
    // sees (All Layers, flattened) -- the same colour mode as the layer.
    std::shared_ptr<const pixel::TileMap> source;
    double     dx = 0, dy = 0;
    // s017: the tip -- its shape, its angle (degrees, counter-clockwise) and
    // its ROUNDNESS (1 a circle .. 0.05 a sliver: a flat, calligraphic
    // brush). TILT: the pen's lean turns the tip to face where it leans and
    // flattens it the further it leans (a pencil's side), over angle / roundness.
    Tip        tip = Tip::Round;
    double     angle = 0;
    double     roundness = 1;
    bool       tilt = false;
    // s017: SMUDGE strength 0..1 (Photoshop's default 0.5): how much of the
    // colour the finger carries it lays down, and keeps.
    double     strength = 0.5;
    Dynamics   dyn;                // s028
    Texture    texture;            // s033
    double spacing() const { return std::max(0.5, size * 0.25); }
};

// s015: where a Clone Stamp stroke takes from. Alt- / Ctrl-click sets the
// source point; a stroke's offset is (source - where the stroke starts).
// ALIGNED (Photoshop's default): the first stroke after setting the source
// fixes the offset and later strokes keep it (the source walks with you);
// not aligned: every stroke starts again from the source point.
struct CloneSource {
    bool       set = false;
    select::Pt point;
    bool       aligned = true;
    bool       fixed = false;   // aligned: an offset is in use
    double     dx = 0, dy = 0;
    void set_point(select::Pt p) { set = true; point = p; fixed = false; }
    // A stroke starts at p: the offset it paints with (set must be true).
    void begin(select::Pt p) {
        if (!aligned || !fixed) { dx = point.x - p.x; dy = point.y - p.y; fixed = true; }
    }
    // Where the source is while the brush is at p (the canvas's crosshair).
    select::Pt at(select::Pt p) const { return fixed ? select::Pt{p.x + dx, p.y + dy} : point; }
};

// A dab: where, the pen's pressure there (1 for a mouse) and (s017) its tilt
// (-1 .. 1 each way, GDK's; 0 for a mouse). s028: what the dynamics drew for
// it -- a size scale, a turn (degrees, added to the tip's angle), a roundness
// scale, and (tinted) its own colour in the document's mode.
struct Dab {
    double x = 0, y = 0; float p = 1.f; float tx = 0.f, ty = 0.f;
    float size = 1.f, turn = 0.f, round = 1.f;
    bool  tinted = false;
    pixel::Px colour{};
};

// s028: the DYNAMICS' dice, one per stroke. apply() turns the dabs spaced
// along the path into the dabs painted: `count` of them at each, scattered,
// each with its own size / angle / roundness (and colour) from the brush's
// dynamics. A brush without dynamics gets its dabs back untouched. The same
// seed and the same dabs give the same stroke (the selftest's replay).
class Jitter {
public:
    Jitter() = default;
    explicit Jitter(std::uint64_t seed) : m_s(seed ? seed : 1) {}
    // s028 m2: the colours colour jitter works from -- encoded sRGB 0..1 --
    // and the document's format (each dab's colour is made in its mode).
    void colours(const pixel::Format& f, const float fg[3], const float bg[3]);
    std::vector<Dab> apply(const std::vector<Dab>& path, const Brush& b);
    double next();   // 0 .. 1
private:
    std::uint64_t m_s = 0x6b6c696d74ULL;
    bool   m_have_last = false;
    double m_lx = 0, m_ly = 0, m_dx = 1, m_dy = 0;   // the path's direction
    bool   m_colours = false;
    pixel::Format m_format;
    float  m_fg[3]{0, 0, 0}, m_bg[3]{1, 1, 1};
    bool   m_stroke_drawn = false;   // per_tip off: the stroke's one colour
    pixel::Px m_stroke_colour{};
    pixel::Px colour_of(const Brush& b);
};
// s028 m2: HSB jitter of an sRGB colour: hue turned by dh (turns, -0.5..0.5),
// saturation and brightness moved by ds / dv (clamped 0..1).
void jitter_hsb(float rgb[3], double dh, double ds, double dv);

// s017: a dab's strength at (dx, dy) from its centre -- radius r, the
// brush's tip / angle / roundness / hardness (and the dab's tilt when the
// brush follows it); (ix, iy) the image pixel, for Chalk's grain.
float tip_alpha(double dx, double dy, double r, const Brush& b, const Dab& d, int ix, int iy);
// How far from its centre a dab of radius r can reach (a square's corner).
double tip_reach(double r, const Brush& b);
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
    struct Dab m_last_dab;
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
double dab_radius(const Brush& b, const Dab& d);   // s028: x the dab's size jitter
float  dab_strength(const Brush& b, float p);

// The dab's strength at distance d from its centre (radius r = size / 2).
float dab_alpha(double d, const Brush& b);
float dab_alpha(double d, double r, double hardness);   // s009: at a given radius

// Coverage += the dabs (by max). `touched` (on the coverage's grid) gains
// every tile a dab reached. Coverage is F32 Gray, no alpha, at the image size.
pixel::TileMap add_dabs(const pixel::TileMap& cov, const Brush& b, const std::vector<select::Pt>& dabs, TileSet& touched);
pixel::TileMap add_pen_dabs(const pixel::TileMap& cov, const Brush& b, const std::vector<Dab>& dabs, TileSet& touched);
pixel::TileMap new_coverage(int w, int h);
// s028 m2: COLOR DYNAMICS -- the stroke's own colour, where its dabs differ.
// A TINT map (F32, the layer's colour mode, no alpha) beside the coverage:
// each dab paints its colour (a tinted dab's own, else the brush's) over what
// the stroke laid there before, by its strength -- later dabs on top, as in
// Photoshop's stroke -- while the coverage still rises by max / flow as ever.
// apply() then takes each pixel's colour from the tint. Only the Brush makes
// one (not the Eraser, a sampled tool, Smudge, a mask or Quick Mask).
pixel::TileMap new_tint(const pixel::Format& layer, int w, int h);
pixel::TileMap add_pen_dabs(const pixel::TileMap& cov, const Brush& b, const std::vector<Dab>& dabs, TileSet& touched,
                            pixel::TileMap* tint);

// The layer as the stroke has it: `current` with each `touched` tile
// recomputed from `original`, `cov`, the selection `mask` and the brush.
pixel::TileMap apply(const pixel::TileMap& original, const pixel::TileMap& current, const pixel::TileMap& cov,
                     const pixel::TileMap* mask, const Brush& b, const TileSet& touched,
                     const pixel::TileMap* tint = nullptr);   // s028 m2

// s015: a clone source for this layer: its own pixels (`all` false) or the
// flattened picture -- refused ("" back in `why`) when the flattened picture
// isn't the layer's colour mode (All Layers: RGB documents, for now).
std::shared_ptr<const pixel::TileMap> clone_source(const Document& d, const Document::Id& id, bool all, std::string& why);

// May this layer be painted? "" yes, else why not (for a toast).
std::string refusal(const Document& d, const Document::Id& id, const Brush& b);

// A whole stroke, synchronously, as ONE step (the verb table; tests). The
// app does the same in pieces on workers (Shell_paint.cpp).
bool stroke(DocJournal& j, Document& d, const Document::Id& id, const std::vector<select::Pt>& path, Brush b);
// s017: SMUDGE. The finger carries a patch of colour (premultiplied, the
// dab's size) taken where the stroke starts; each dab lays it over what is
// there by (tip x pressure x opacity x selection) x strength, then picks up
// the result -- so colour drags along and fades. Unlike the Brush this can't
// be recomputed from the original: dabs run in order on the CURRENT pixels,
// and the carry lives across calls (the app: one job at a time).
struct SmudgeCarry {
    std::vector<float> c;   // side x side x channels, premultiplied
    int  side = 0, channels = 0;
    bool started = false;
};
pixel::TileMap smudge(const pixel::TileMap& current, const pixel::TileMap* mask, const Brush& b,
                      const std::vector<Dab>& dabs, SmudgeCarry& carry, TileSet& touched);
// A whole Smudge stroke as ONE step ("Smudge").
bool stroke_smudge(DocJournal& j, Document& d, const Document::Id& id, const std::vector<Dab>& path, Brush b);

// s016: a QUICK MASK stroke -- the selection painted (b.colour.c[0]: 1 selects,
// 0 masks), no selection counting as all selected. ONE step.
bool stroke_selection(DocJournal& j, Document& d, const std::vector<Dab>& path, Brush b);
// s009 m3: a stroke with the pen's pressure at each point.
bool stroke_pen(DocJournal& j, Document& d, const Document::Id& id, const std::vector<Dab>& path, Brush b);
// s028 m2: with the dice given -- colour dynamics need jit.colours() set.
bool stroke_pen(DocJournal& j, Document& d, const Document::Id& id, const std::vector<Dab>& path, Brush b, Jitter jit);

}  // namespace klimt::core::paint
