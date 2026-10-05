#pragma once
#include <memory>
#include <string>
#include <vector>

#include "core/Image.h"

// How a source file's pixels are decoded (Image Input's settings plus the project's working space).
struct DecodeOptions {
    bool srgbToLinear = false;  // PNG/JPG...: decode RGB from the sRGB curve to linear light
    // Scene-linear project: JPEGs are turned upright from EXIF, and RAW stays linear. Legacy
    // projects load files as stored, and RAW is encoded to sRGB 0..1 like any other photo.
    bool sceneLinear = false;
    int rawHighlights = 2;  // raw::Highlights (Reconstruct, Image Input's default)
    // With srgbToLinear: decode through the file's embedded ICC profile (Display P3, Adobe RGB...)
    // to linear Rec.709 instead of assuming sRGB. sRGB-tagged and untagged files are unaffected.
    bool embeddedProfile = false;
    bool operator==(const DecodeOptions&) const = default;
};

// Loads PNG/JPG/TIFF/BMP/TGA (8 or 16 bit) as float RGBA 0..1, float TIFF and OpenEXR as stored
// (scene-linear; see finishLinearData), or a camera RAW (see raw::load). With
// srgbToLinear, RGB is decoded from the sRGB curve to linear light (alpha is not). preview allows
// a faster, smaller decode (half-size RAW); fullW/fullH receive the full-resolution size either
// way. Returns null and sets err on failure.
std::shared_ptr<Image> loadImage(const std::string& pathU8, std::string& err, const DecodeOptions& opt,
                                 bool preview = false, int* fullW = nullptr, int* fullH = nullptr);
inline std::shared_ptr<Image> loadImage(const std::string& pathU8, std::string& err, bool srgbToLinear = false) {
    return loadImage(pathU8, err, DecodeOptions{srgbToLinear});
}

// What DecodeOptions::embeddedProfile does with a file, for the Inspector: empty when it has no
// profile or an sRGB one, otherwise the profile's name, or why it can't be used. Cached per path.
std::string embeddedProfileInfo(const std::string& pathU8);

// Files of scene-linear float data (OpenEXR, float TIFF): Image Input reads them as Linear
// Rec.709, as Blender does.
bool isLinearImageFile(const std::string& pathU8);

// File dialog filter and drop check for every format loadImage reads.
extern const char* const kImageFileFilter;
bool isImageFile(const std::string& pathU8);

// Saves 8-bit PNG or JPG depending on extension (RGB when alpha is fully opaque). Returns false and
// sets err on failure.
bool saveImage(const std::string& pathU8, const Image& img, std::string& err, int jpegQuality = 95);

// Decodes an 8-bit image file held in memory (an embedded JPEG preview, a stored thumbnail) as
// float RGBA 0..1, values as stored (display-encoded). Null and err on failure.
std::shared_ptr<Image> decodeImageMemory(const unsigned char* data, size_t len, std::string& err);
// Encodes display-encoded 0..1 RGB as a JPEG file's bytes (alpha is dropped); empty on failure.
std::vector<unsigned char> encodeJpegMemory(const Image& img, int quality = 85);

// Box-filter downscale so the longest edge is <= maxEdge. Returns the input if already small enough.
std::shared_ptr<const Image> downscaleToFit(const std::shared_ptr<const Image>& src, int maxEdge);
