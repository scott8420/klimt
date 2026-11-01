#pragma once
// core::psd::adj -- ADJUSTMENT LAYERS in Photoshop's own blocks, both ways
// (s053; WEAKNESSES 58). docs/file-format.md "Photoshop adjustment layers".
//
// Klimt's adjustments and the tagged blocks Photoshop keeps them in:
//
//   Levels               levl  v2: 29 records (in floor, in ceiling, out
//                              floor, out ceiling, gamma x 100); 0 = RGB, 1..3 = R G B
//   Curves               curv  v1: a channel bitmap, each channel's points
//                              (output, input; 0..255), then "Crv " v4 again
//                              with channel ids (what Photoshop CS+ reads)
//   Hue/Saturation       hue2  v2: Colorize, its hue / sat / light, the master
//                              hue / sat / light, six colour ranges
//   Brightness/Contrast  CgEd  a descriptor: Brgh, Cntr, useLegacy false (the
//                              modern maths, Klimt's); plus "brit" for old readers
//   Invert               nvrt  no data                       (s061)
//   Posterize            post  levels (2 bytes), 2 bytes padding
//   Threshold            thrs  level (2 bytes), 2 bytes padding
//   Exposure             expA  version 1; exposure, offset, gamma (floats)
//
// Pure: no files, no GTK. The reader feeds a layer's blocks one by one and
// asks what they made; the writer asks for the blocks of one Adjust.
#include "core/Adjust.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace klimt::core::psd::adj {

using Bytes = std::vector<std::uint8_t>;

struct Block {
    std::string key;   // four characters
    Bytes       data;  // unpadded
};

// The blocks one adjustment layer carries, in Photoshop's order. Empty for
// AdjustKind::None.
std::vector<Block> blocks(const Adjust& a);

// One layer record's blocks, read.
class Reading {
public:
    // Hand over every tagged block of the record; true when it was one of the
    // adjustment blocks read here (levl, curv, hue2, brit, CgEd; s061: nvrt,
    // post, thrs, expA).
    bool feed(std::string_view key, const std::uint8_t* d, std::size_t n);
    // What the blocks made: nullopt when they hold nothing Klimt can (or are
    // damaged). The result is normalised -- a Document takes it as it is.
    std::optional<Adjust> result() const;
    // What didn't cross exactly, as short phrases the toast counts:
    // "colour ranges" (Hue/Saturation's per-range settings), "legacy"
    // (Brightness/Contrast's old maths, read the modern way), "channels"
    // (Levels / Curves on a channel Klimt doesn't have).
    std::vector<std::string> losses() const;

private:
    std::optional<Adjust> m_a;
    bool m_brit = false, m_cged = false, m_legacy = false, m_ranges = false, m_channels = false;
    int  m_brit_b = 0, m_brit_c = 0;
};

}  // namespace klimt::core::psd::adj
