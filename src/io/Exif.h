#pragma once
// Just enough EXIF to stand photos upright.
#include <memory>
#include <string>

#include "core/Image.h"

namespace exif {

// The EXIF Orientation tag (1..8) of a JPEG file, or 1 when there is none.
int jpegOrientation(const std::string& pathU8);

// The image turned upright for an EXIF orientation (1 = already upright: returns the input).
std::shared_ptr<Image> applyOrientation(const std::shared_ptr<Image>& img, int orientation);

}  // namespace exif
