#pragma once
// core::profile -- ASSIGN and CONVERT to Profile (s068; WEAKNESSES 14).
// docs/colour.md. GTK-free.
//
//   * ASSIGN: the numbers stay, what they mean changes -- the document's
//     profile replaced (or taken away: then its mode's plain meaning). Only
//     a profile of the document's own colour space. One step.
//   * CONVERT: the look stays, the numbers change -- every pixel layer's
//     pixels (its pixels beyond the image too) taken from the document's
//     profile (none: the mode's plain meaning, tint::implicit) to `to`,
//     relative colorimetric + black point compensation; the document's MODE
//     becomes `to`'s colour space (RGB, Gray, CMYK, Lab) -- Image > Mode is
//     a Convert. Depth and alpha kept. Masks, saved selections and the
//     selection are not colour and stay. Text layers are set again in the
//     new mode (canvas::land). One step; the plan runs on a worker, on the
//     cores.
#include "core/Canvas.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace klimt::core::profile {

using IccPtr = std::shared_ptr<const std::vector<std::uint8_t>>;

// "" when `icc` can be assigned to `im` (readable, of its mode's space),
// else why not. nullptr (none) always can.
std::string assign_refusal(const Image& im, const IccPtr& icc);
bool assign(DocJournal& j, Document& d, const IccPtr& icc);

// "" when `to` is a profile Klimt can convert into (readable; RGB, Gray,
// CMYK or Lab), else why not.
std::string convert_refusal(const Image& im, const IccPtr& to);
std::optional<canvas::Reshape> convert_plan(const canvas::Before& b, const IccPtr& to,
                                            const std::function<bool()>& cancelled = {},
                                            const std::function<void(double)>& progress = {});
bool convert(DocJournal& j, Document& d, const IccPtr& to);   // plan + land, for the tests

// The pixel mode a profile's colour space gives (nullopt: not one of ours).
std::optional<pixel::Mode> mode_of(const std::vector<std::uint8_t>& icc);

// s068 m2: the profiles the dialogs offer -- Klimt's own (sRGB, Adobe RGB,
// Gray Gamma 2.2, Gray (sRGB curve), Plain CMYK, Lab) and every readable
// .icc / .icm in the system's colour folders (/usr/share/color/icc,
// ~/.local/share/icc, ~/.color/icc -- or `dirs`), by mode, then name. Read
// once (the folders: on first ask).
struct Choice {
    std::string name;
    IccPtr      icc;
    pixel::Mode mode = pixel::Mode::RGB;
    std::string path;   // "" for Klimt's own
};
std::vector<Choice> catalog(const std::vector<std::string>& dirs = {});
// Image > Mode's profile for each mode: sRGB, Gray Gamma 2.2, Plain CMYK, Lab.
IccPtr builtin(pixel::Mode m);
// A profile file the user picked. nullopt and `why` when it isn't one.
std::optional<Choice> load(const std::string& path, std::string& why);

}  // namespace klimt::core::profile
