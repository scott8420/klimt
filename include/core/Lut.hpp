#pragma once
// core/Lut -- a COLOUR LOOKUP's table (s066; WEAKNESSES 34). docs/adjustments.md.
//
// A 3D lookup table: n x n x n samples of the colour cube (red changing
// fastest -- the .cube order), each the colour that sample turns into; sRGB
// 0..1 in and out. A colour between samples is mixed from the eight round it
// (trilinear). Comes from a .cube file (Adobe / Resolve: 3D, or 1D expanded
// to 3D) or from one of Klimt's own looks (made here, no licence needed).
// GTK-free.
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace klimt::core {

struct Lut {
    std::string name;          // the file's TITLE (or its name), or the look's
    int n = 0;                 // samples a side, 2..kMaxLut
    std::vector<float> rgb;    // n^3 x 3
    bool operator==(const Lut&) const = default;
};
inline constexpr int kMaxLut = 65;
inline constexpr int kLookSize = 33;

namespace lut {
// A .cube's text: LUT_3D_SIZE (or LUT_1D_SIZE, made 3D), DOMAIN_MIN / MAX,
// TITLE, the rows. nullopt and `why` when it isn't one.
std::optional<Lut> parse_cube(std::string_view text, std::string& why);
std::optional<Lut> load_cube(const std::string& path, std::string& why);
// Klimt's own looks, by name (menu order); an unknown name: an empty Lut.
const std::vector<std::string>& looks();
Lut look(std::string_view name);
// The colour through the table, in place (0..1, clamped first).
void apply(const Lut& t, float& r, float& g, float& b);
// For the .klimt: the samples as 16-bit, base64; and back (checked).
std::string encode(const Lut& t);
std::optional<Lut> decode(const std::string& name, int n, std::string_view data);
// "" when well formed.
std::string refusal(const Lut& t);
}  // namespace lut

}  // namespace klimt::core
