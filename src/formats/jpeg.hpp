#pragma once

#include "formats/tga.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::jpeg {

// Decodes a baseline JPEG to the game's own pixel values (it uses Intel's ijl15.dll on SSE processors):
// AP-922 integer inverse DCT, chroma repeated over the pixels it covers and integer YCbCr to RGB. Only
// what the game's pictures use is read: baseline Huffman, 8-bit, grey or YCbCr in one scan, sampling
// factors 1 or 2. Anything else, broken data or a side over `maxSide` gives nullopt. Alpha is 255.
[[nodiscard]] std::optional<tga::Pixels> decode(std::span<const std::uint8_t> data, std::uint32_t maxSide = tga::kMaxSide);

// Encodes a picture as a baseline JFIF JPEG (YCbCr, not subsampled, the standard tables scaled to
// `quality`, 1 to 100), as the game saves snapshots. Alpha is left out. Empty for an empty or
// malformed picture, or one over 65535 pixels on a side.
[[nodiscard]] std::vector<std::uint8_t> encode(const tga::Pixels& picture, int quality = 90);
// The same with its alpha in the game's "ALFA" segment after the JFIF header, as the game keeps the
// pictures of families (image::decode reads it back). Without it when it doesn't fit one segment.
[[nodiscard]] std::vector<std::uint8_t> encodeWithAlpha(const tga::Pixels& picture, int quality = 90);

} // namespace ts2::jpeg
