#pragma once
#include <memory>
#include <string>

#include "core/Image.h"

// How a source file's pixels are decoded (Image Input's settings plus the project's working space).
struct DecodeOptions {
    bool srgbToLinear = false;  // PNG/JPG...: decode RGB from the sRGB curve to linear light
    // Scene-linear project: JPEGs are turned upright from EXIF, and RAW stays linear. Legacy
    // projects load files as stored, and RAW is encoded to sRGB 0..1 like any other photo.
    bool sceneLinear = false;
    int rawHighlights = 2;  // raw::Highlights (Reconstruct, Image Input's default)
    bool operator==(const DecodeOptions&) const = default;
};

// Loads PNG/JPG/BMP/TGA (8 or 16 bit) as float RGBA 0..1, or a camera RAW (see raw::load). With
// srgbToLinear, RGB is decoded from the sRGB curve to linear light (alpha is not). preview allows
// a faster, smaller decode (half-size RAW); fullW/fullH receive the full-resolution size either
// way. Returns null and sets err on failure.
std::shared_ptr<Image> loadImage(const std::string& pathU8, std::string& err, const DecodeOptions& opt,
                                 bool preview = false, int* fullW = nullptr, int* fullH = nullptr);
inline std::shared_ptr<Image> loadImage(const std::string& pathU8, std::string& err, bool srgbToLinear = false) {
    return loadImage(pathU8, err, DecodeOptions{srgbToLinear});
}

// File dialog filter and drop check for every format loadImage reads.
extern const char* const kImageFileFilter;
bool isImageFile(const std::string& pathU8);

// Saves 8-bit PNG or JPG depending on extension (RGB when alpha is fully opaque). Returns false and
// sets err on failure.
bool saveImage(const std::string& pathU8, const Image& img, std::string& err, int jpegQuality = 95);

// Box-filter downscale so the longest edge is <= maxEdge. Returns the input if already small enough.
std::shared_ptr<const Image> downscaleToFit(const std::shared_ptr<const Image>& src, int maxEdge);
