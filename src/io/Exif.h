#pragma once
// Just enough EXIF to stand photos upright and carry capture details into exports.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Image.h"

namespace exif {

// The EXIF Orientation tag (1..8) of a JPEG file, or 1 when there is none.
int jpegOrientation(const std::string& pathU8);

// The image turned upright for an EXIF orientation (1 = already upright: returns the input).
std::shared_ptr<Image> applyOrientation(const std::shared_ptr<Image>& img, int orientation);

// EXIF for an image exported from `sourceU8` at w x h, as a TIFF structure (the APP1 payload
// after "Exif\0\0"); empty when the source has none.
// - JPEG sources: their EXIF block, with Orientation set to 1 (the pixels are already upright),
//   the pixel dimensions updated, and the embedded thumbnail unlinked (it shows the unedited photo).
// - RAW sources: camera, lens, exposure settings and capture time read by LibRaw.
std::vector<uint8_t> exportBlock(const std::string& sourceU8, int w, int h);

}  // namespace exif
