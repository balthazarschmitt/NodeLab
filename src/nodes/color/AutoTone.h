#pragma once
#include <array>

#include "core/Image.h"

// Lightroom's Basic > Auto: Tone settings for the Basic node that spread an image's tones,
// worked out from the image arriving at the node. The Basic node itself is run on a small copy
// while searching, so the result matches what the sliders actually do in either working space.
namespace autotone {

struct Settings {
    float exposure = 0, contrast = 0, highlights = 0, shadows = 0, whites = 0, blacks = 0;
};

// `linear`: the project is scene-linear (else legacy sRGB-encoded values).
Settings compute(const Image& src, bool linear);

// Basic's param indices for each field, in Settings order (Exposure .. Blacks).
constexpr std::array<int, 6> kBasicParams = {3, 4, 5, 6, 7, 8};

}  // namespace autotone
