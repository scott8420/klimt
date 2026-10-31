#pragma once
// core::psd -- Photoshop documents in (s050; REQUIREMENTS "Added" 3;
// docs/file-format.md "Photoshop (.psd)"). Read only: Klimt saves its own
// .klimt and exports ORA for layered exchange.
//
// What crosses: pixel layers at their places, groups (open or closed, Pass
// Through kept), names (the Unicode one when present), visibility, opacity,
// Fill, blend modes, clipping, layer masks (on, off, their default colour
// outside their box), 8- and 16-bit RGB; resolution and the colour profile.
// Raw, RLE (PackBits), ZIP and ZIP-with-prediction channels. A file with no
// layers opens as its merged picture, one "Background".
//
// What doesn't is said in the open's toast, as ORA does: adjustment and fill
// layers (left out), layer styles (left out), vector masks (left out), text
// and smart objects (read as their pixels). Not read at all, refused in
// words: PSB (large documents), CMYK / Gray / Lab / Indexed / Bitmap, 32-bit.
// Pure over its arguments: runs on a worker.
#include "core/File.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace klimt::core::psd {

std::optional<file::Contents> open(const std::string& path, std::string& error, const formats::Progress& p = {});

// Photoshop's four-character blend key ("mul ", "pass") -> Klimt's mode;
// nullopt when Klimt has no such mode. And back (the selftest writes PSDs).
std::optional<Blend> blend_of_key(std::string_view key);
std::string_view     key_of_blend(Blend b);

}  // namespace klimt::core::psd
