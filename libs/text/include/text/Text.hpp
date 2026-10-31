#pragma once
// libs/text -- Klimt's TYPE (s046): a live text layer's settings and how they
// are set, on Pango + Cairo (no GTK -- core stays GTK-free and headless).
//
// A text layer is a Spec. The document keeps the Spec and the pixels it
// renders to; an edit makes a new Spec and the pixels are drawn again from it
// (core/Text.hpp). Nothing here knows the document: everything is in IMAGE
// PIXELS, through the Spec's matrix.
//
// Geometry. The text is laid out in TEXT SPACE, then `m` maps text space to
// the image (identity for text that was only typed; Crop, Canvas Size, Image
// Size and the canvas rotations compose into it, so the type stays live).
//   * POINT text (box_w == 0): one line per paragraph, never wrapped. (x, y)
//     is the ANCHOR -- the first line's BASELINE, at its left, centre or right
//     end by the alignment. Photoshop's point type.
//   * BOX text (box_w > 0): (x, y) is the box's top-left; lines wrap at box_w,
//     align (or justify) inside it; box_h > 0 clips what overflows it.
//
// Thread-safe: every call makes its own Pango context on the thread's own
// font map, so a worker may render while the UI thread hit-tests.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace klimt::text {

enum class Align : std::uint8_t { Left, Center, Right, Justify };

struct Spec {
    std::string text;                 // UTF-8; '\n' breaks a paragraph
    std::string family = "Sans";      // a font family (Pango falls back when missing)
    int         weight = 400;         // 100..900 (400 Regular, 700 Bold)
    bool        italic = false;
    double      size   = 48;          // text-space px (the em)
    std::array<float, 4> colour{0, 0, 0, 1};   // sRGB 0..1, straight
    Align       align  = Align::Left;
    double      tracking = 0;         // 1/1000 em between letters (Photoshop's tracking)
    double      leading  = 0;         // line pitch in px; 0 = the font's own (auto)
    double      x = 0, y = 0;         // the anchor (point) or the box's top-left
    double      box_w = 0, box_h = 0; // 0: point text
    std::array<double, 6> m{1, 0, 0, 1, 0, 0};   // text -> image: (a b c d e f), x' = a x + c y + e, y' = b x + d y + f
    bool point() const { return !(box_w > 0); }
    bool operator==(const Spec&) const = default;
};

struct Box { double x = 0, y = 0, w = 0, h = 0; };   // a plain struct (no std::pair of doubles: RULES)
struct IRect { int x = 0, y = 0, w = 0, h = 0; bool empty() const { return w <= 0 || h <= 0; } };

// What a Spec refuses, or "" -- size 1..5000 px, weight 100..900, tracking
// -1000..5000, leading 0..10000, a box no wider than 100000, a matrix that is
// finite and not degenerate, valid UTF-8.
std::string refusal(const Spec& s);
// A Spec put in range (the fields clamped; bad UTF-8 dropped; a degenerate
// matrix back to identity). refusal(normalised(s)) is "".
Spec normalised(Spec s);
// A uniform scale held in the matrix (its linear part k * I, k > 0) folded
// into size, tracking-free fields and the box -- what Image Size leaves, so the
// size field says what the eye sees. Any other matrix: unchanged.
Spec folded(Spec s);
// The matrix `a` composed in front of the Spec's (image' = a(image)).
Spec mapped(Spec s, const std::array<double, 6>& a);

// ── setting it ──────────────────────────────────────────────────────────────
// The text drawn as COVERAGE (0..255, one byte a pixel) over the image
// rectangle it touches, clipped to [0, clip_w) x [0, clip_h). The colour is
// the caller's to apply. Empty text or nothing on the image: an empty rect.
struct Coverage {
    IRect                     at;     // image pixels
    std::vector<std::uint8_t> a;      // at.w * at.h, row-major
};
Coverage render(const Spec& s, int clip_w, int clip_h);

// The rectangle the text's lines take (logical, not ink), image pixels, the
// axis-aligned bounds through the matrix. Empty text: the caret's line.
Box bounds(const Spec& s);
// Box type with more text than fits its height (the lines that don't fit
// whole aren't set) -- the editor marks the frame.
bool overflows(const Spec& s);
// The four corners of the text's frame in the image (the box for box text;
// the lines' logical extent for point text), clockwise from top-left.
std::array<Box, 4> frame(const Spec& s);   // each Box's (x, y) a corner; w = h = 0

// ── editing on the canvas ───────────────────────────────────────────────────
// Byte indices into Spec::text, always on a character boundary.
struct Caret { double x0 = 0, y0 = 0, x1 = 0, y1 = 0; };   // the caret's line, image pixels (top -> bottom)
Caret caret(const Spec& s, int index);
int   index_at(const Spec& s, double x, double y);         // the nearest boundary to an image point
// One character left (dir < 0) or right, visually (bidi-aware); clamped.
int   step(const Spec& s, int index, int dir);
// One line up (dir < 0) or down, keeping the caret's x; clamped to the ends.
int   line_step(const Spec& s, int index, int dir);
// The start / end of the caret's line.
int   line_home(const Spec& s, int index);
int   line_end(const Spec& s, int index);
// Word edges (Ctrl+arrows, double-click): the previous / next word boundary.
int   word_step(const Spec& s, int index, int dir);
// The selection [a, b) as image-space quads (one per line it crosses), each
// as four corners clockwise from top-left.
std::vector<std::array<Box, 4>> range(const Spec& s, int a, int b);

// UTF-8 helpers the editor shares: the boundary before / after `i`.
int prev_char(const std::string& t, int i);
int next_char(const std::string& t, int i);

// The installed font families, sorted, without duplicates.
std::vector<std::string> families();
// Does the font map hold this family (case-insensitive)?
bool has_family(const std::string& f);

}  // namespace klimt::text
