#pragma once
// The viewer's display conversion on the device: the view transform, the 8-bit bytes with
// clipping warnings, and the histogram in one compute pass. On the CPU these took 30-60 ms per
// megapixel (as long as a whole GPU evaluation), and a GPU result also had to be downloaded as
// floats first; here only the bytes and 1 KB of histogram come back.
#include <cstdint>
#include <vector>

#include "core/ColorManagement.h"
#include "core/Value.h"

namespace gpu {

struct DisplayResult {
    int w = 0, h = 0;
    std::vector<unsigned char> bytes;  // RGBA8, as displayBytes() makes them
    // When asked for: the r, g, b and luminance bins (256 each), then the clip-high and clip-low
    // flags, counted as Histogram::compute does (rows skipped on images over 2 MP).
    std::vector<uint32_t> histogram;
};

// `scene` is an image or channel on the device (a channel shows as grey), or a CPU image or
// channel, which is uploaded. Needs a Scope; throws gpu::Error on device failures.
DisplayResult display(const Value& scene, const ColorManagement& cm, bool clipping, bool histogram);

}  // namespace gpu
