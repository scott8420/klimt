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
#include "tint/Tint.hpp"   // s070: Render

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
// s070: `r` -- the rendering intent and BPC. An untagged document's numbers
// are taken as the working space's (tint::working), else tint::implicit.
std::optional<canvas::Reshape> convert_plan(const canvas::Before& b, const IccPtr& to, tint::Render r = {},
                                            const std::function<bool()>& cancelled = {},
                                            const std::function<void(double)>& progress = {});
bool convert(DocJournal& j, Document& d, const IccPtr& to, tint::Render r = {});   // plan + land, for the tests

// The pixel mode a profile's colour space gives (nullopt: not one of ours).
std::optional<pixel::Mode> mode_of(const std::vector<std::uint8_t>& icc);

// s068 m2: the profiles the dialogs offer -- Klimt's own (sRGB, Adobe RGB,
// Gray Gamma 2.2, Gray (sRGB curve), Plain CMYK, Lab) and every readable
// .icc / .icm in the system's colour folders (/usr/share/color/icc,
// ~/.local/share/icc, ~/.color/icc -- or `dirs`), by mode, then name. Read
// once (the folders: on first ask). s069: Ghostscript's folder too
// (/usr/share/ghostscript -- its SWOP CMYK, a gray, Adobe-RGB-alike).
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

// s069: VIEW > PROOF SETUP's profile, from what the prefs remember: a file's
// path, or one of Klimt's own by name. "" (or one gone): the default -- a
// SWOP press from the system's folders when there is one (Ghostscript ships
// "Artifex CMYK SWOP Profile", near Photoshop's U.S. Web Coated (SWOP)),
// else Plain CMYK. `remembered_as` is what to keep for a choice.
// s070: the default is the working CMYK when one is set (Photoshop's
// "Working CMYK" proof).
Choice proof_choice(const std::string& remembered, const std::vector<std::string>& dirs = {});
std::string remembered_as(const Choice& c);
// s070: a remembered profile found again (catalog, or the file); nullopt:
// "" or gone.
std::optional<Choice> find(const std::string& remembered, const std::vector<std::string>& dirs = {});
// s070: Image > Mode's profile -- the working space for Gray / CMYK when set,
// else builtin().
IccPtr mode_profile(pixel::Mode m);

// s071: THE MONITOR's profile, from colord's display devices (read over
// D-Bus by the shell): the device on `connector` (its "XRANDR_name", e.g.
// "DP-1" -- GDK's connector name) with a profile; else the primary one;
// else the first with any. Its default profile's file ("" -- none).
struct MonitorDevice {
    std::string connector;
    bool        primary = false;
    std::vector<std::string> profiles;   // files, the default first
};
std::string monitor_profile(const std::vector<MonitorDevice>& devices, const std::string& connector);

}  // namespace klimt::core::profile
