#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/Image.h"

// A PNG decoder for the common layouts of photos and renders: 8- and 16-bit greyscale, grey +
// alpha, RGB and RGBA, not interlaced, without tRNS. It inflates with zlib-ng, which is several
// times faster than stb_image's inflate, and gives exactly what stb_image's 4-channel load gives.
// Anything else (palettes, low bit depths, interlacing, transparency keys, damaged files) returns
// false, and the caller falls back to stb_image, so its results and errors stay the same.
namespace png {

struct Decoded {
    int w = 0, h = 0;
    int channels = 0;  // 1 grey, 2 grey + alpha, 3 RGB, 4 RGBA
    bool sixteen = false;
    size_t stride = 0;  // bytes per row in raw, its filter byte included
    std::vector<uint8_t, UninitAllocator<uint8_t>> raw;  // the unfiltered rows, each after its filter byte

    // Row y's RGBA codes (w * 4), as stb's 4-channel load gives them: grey repeated in R, G and
    // B, and opaque alpha where the file has none. Use the overload for the file's depth.
    void expandRow(int y, uint8_t* rgba) const;
    void expandRow(int y, uint16_t* rgba) const;
};

bool decode(const uint8_t* file, size_t size, Decoded& out);

}  // namespace png
