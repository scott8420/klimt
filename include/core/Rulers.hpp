#pragma once
// core::rulers -- RULERS and UNITS (s031). GTK-free: the units, conversions,
// and where a ruler's ticks and labels go for a view -- the RulerView only
// draws what this says. docs/rulers.md.
//
// Positions are IMAGE pixels; a ruler's numbers count from the document's
// zero point (Image::origin_x / _y) in the chosen unit. Physical units use
// the document's resolution; Percent is of the image's width (the top ruler)
// or height (the left one).
#include "core/Document.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace klimt::core::rulers {

enum class Unit : std::uint8_t { Pixels, Inches, Centimetres, Millimetres, Points, Picas, Percent };
const std::vector<Unit>& units();             // the menu's order
std::string_view label(Unit u);               // "Centimetres"
std::string_view key(Unit u);                 // "cm" -- in prefs, and after numbers
Unit from_key(std::string_view k);            // unknown: Pixels

// Image pixels in one unit, at `dpi`; `extent` is the image's side the
// ruler measures (Percent).
double px_per_unit(Unit u, double dpi, double extent);
// A length or position in image pixels, in the unit, as a short label
// ("12.5 cm", "300 px").
std::string format(double px, Unit u, double dpi, double extent, bool with_unit = true);

struct Tick {
    double at = 0;        // image pixels
    int    level = 0;     // 0: a major tick (labelled); 1: a half; 2: a minor
    std::string label;    // majors only: the value from the zero point
};
// The ticks for image positions from..to, at `zoom` screen px per image px,
// counting from `origin`: majors at a round step (1-2-5 x 10^n; inches also
// halves to sixteenths) at least `min_gap` screen px apart, minors between
// while they stay 4+ screen px apart.
std::vector<Tick> ticks(Unit u, double dpi, double extent, double origin, double from, double to, double zoom,
                        double min_gap = 64);
// The major step chosen, in units (for the selftest).
double major_step(Unit u, double dpi, double extent, double zoom, double min_gap = 64);

}  // namespace klimt::core::rulers

namespace klimt::core::canvas {
// s031: the zero point set (image pixels; Photoshop's ruler origin) -- ONE
// step ("Ruler Origin"). False when unchanged.
bool set_ruler_origin(DocJournal& j, Document& d, double x, double y);

// s032 -- GUIDES, each ONE step: added (at a whole pixel; refused off the
// picture by more than its size, or on one already there), moved (index;
// off the picture: removed), removed, all cleared.
bool add_guide(DocJournal& j, Document& d, Guide g);
bool move_guide(DocJournal& j, Document& d, std::size_t i, double at);
bool remove_guide(DocJournal& j, Document& d, std::size_t i);
bool clear_guides(DocJournal& j, Document& d);
}  // namespace klimt::core::canvas

namespace klimt::core::guides {
// s032: SNAPPING. `v` (image px) pulled to the nearest guide of that
// orientation (or the picture's edges / centre, when `edges`) within `tol`
// image px; else v unchanged. `snapped` (optional) says whether it moved.
double snap(double v, bool vertical, const Image& im, double tol, bool edges = true, bool* snapped = nullptr);
// The guide (index) within `tol` image px of (x, y); -1: none. Vertical
// ones are checked against x, horizontal against y; the nearest wins.
int hit(const Image& im, double x, double y, double tol);
}  // namespace klimt::core::guides
