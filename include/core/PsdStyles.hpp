#pragma once
// core::psd::fx -- LAYER STYLES in Photoshop's "lfx2" descriptor, both ways
// (s053 m2; WEAKNESSES 58). docs/file-format.md "Photoshop layer styles".
//
// lfx2 is an action descriptor (version 0, descriptor version 16) whose items
// are the effects, each an object of its class:
//
//   DrSh drop shadow   IrSh inner shadow   OrGl outer glow   IrGl inner glow
//   ebbl bevel/emboss  ChFX satin          SoFi colour overlay
//   GrFl gradient overlay                  FrFX stroke
//   (patternFill -- pattern overlay: its pattern would have to travel in the
//   file; not written, so a layer with one is baked as before)
//
// Read too from "lmfx" (Photoshop CC's form when an effect is used more than
// once: lists like dropShadowMulti -- the first of each kind is kept).
// Pure: no files, no GTK.
#include "core/Styles.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace klimt::core::psd::fx {

using Bytes = std::vector<std::uint8_t>;

// Can every effect that's on be written live? (Pattern overlay can't.)
bool crosses(const Styles& s);
// The lfx2 block's data for `s` (every effect that's on, and those that are
// off but set -- Photoshop keeps both). Pattern overlay is not written.
Bytes block(const Styles& s);

// One layer record's effects blocks, read.
class Reading {
public:
    // lfx2 / lmfx: true when it was one (read or not).
    bool feed(std::string_view key, const std::uint8_t* d, std::size_t n);
    // What they made, given the document's global light (resources 1037 /
    // 1049; an effect that "uses global light" takes it). nullopt: nothing
    // Klimt can show (none, or damaged).
    std::optional<Styles> result(int global_angle, int global_altitude) const;
    // Phrases the toast counts: "pattern" (a pattern overlay left out),
    // "copies" (an effect used more than once: the first kept), "gradient"
    // (more than two stops / see-through stops / angle, reflected, diamond:
    // its ends, linear), "bevel" (Outer Bevel, Pillow, Stroke Emboss: as the
    // nearest Klimt has), "off" (the styles' master switch off: each kept,
    // switched off).
    std::vector<std::string> losses() const;

private:
    struct Parsed;
    std::shared_ptr<const Parsed> m_p;
};

}  // namespace klimt::core::psd::fx
