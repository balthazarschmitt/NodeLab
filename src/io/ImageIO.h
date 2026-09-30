#pragma once
#include <memory>
#include <string>

#include "core/Image.h"

// Loads PNG/JPG/BMP/TGA (8 or 16 bit) as float RGBA 0..1. Returns null and sets err on failure.
std::shared_ptr<Image> loadImage(const std::string& pathU8, std::string& err);

// Saves 8-bit PNG or JPG depending on extension (RGB when alpha is fully opaque). Returns false and
// sets err on failure.
bool saveImage(const std::string& pathU8, const Image& img, std::string& err, int jpegQuality = 95);

// Box-filter downscale so the longest edge is <= maxEdge. Returns the input if already small enough.
std::shared_ptr<const Image> downscaleToFit(const std::shared_ptr<const Image>& src, int maxEdge);
