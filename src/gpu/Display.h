#pragma once
// The viewer's display conversion on the device: the view transform, the 8-bit bytes with
// clipping warnings, and the histogram in one compute pass. On the CPU these took 30-60 ms per
// megapixel (as long as a whole GPU evaluation), and a GPU result also had to be downloaded as
// floats first; here only the bytes and 1 KB of histogram come back.
#include <cstdint>
#include <vector>

#include "core/ColorManagement.h"
#include "core/Value.h"
#include "gpu/Device.h"

namespace gpu {

struct DisplayResult {
    int w = 0, h = 0;
    std::vector<unsigned char> bytes;  // RGBA8, as displayBytes() makes them
    // Instead of bytes when asked to keep it: the RGBA8 texture, finished and set up for a viewer
    // (linear minification, nearest magnification) in the UI's shared context.
    TexturePtr texture;
    // When asked for: the r, g, b and luminance bins (256 each), then the clip-high and clip-low
    // flags, counted as Histogram::compute does (rows skipped on images over 2 MP).
    std::vector<uint32_t> histogram;
};

// `scene` is an image or channel on the device (a channel shows as grey), or a CPU image or
// channel, which is uploaded. Needs a Scope; throws gpu::Error on device failures.
// keepTexture (only with sharesUiContext()) returns the texture instead of reading it back.
// gamut >= 0 marks colours outside that export space (outspace::Space) magenta: soft proofing's
// gamut warning, for scene-linear projects with the Standard view.
DisplayResult display(const Value& scene, const ColorManagement& cm, bool clipping, bool histogram,
                      bool keepTexture = false, int gamut = -1);

}  // namespace gpu
