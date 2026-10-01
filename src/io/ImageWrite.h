#pragma once
// Export file writers: PNG and TIFF (8 or 16 bit), JPEG, and OpenEXR (half or full float).
#include <cstdint>
#include <string>
#include <vector>

#include "core/Image.h"

// Export formats, in the order of the Export window's and the File Output node's Format menus.
enum class FileFormat { PNG = 0, JPEG = 1, TIFF = 2, EXR = 3 };

struct SaveOptions {
    FileFormat format = FileFormat::PNG;
    // Bits per channel. PNG and TIFF: 8 or 16. OpenEXR: 16 (half float) or 32 (full float).
    // JPEG is always 8.
    int depth = 8;
    int jpegQuality = 95;
    // Tag display formats as sRGB: the sRGB chunk in PNG, an ICC profile in JPEG and TIFF.
    // OpenEXR is always scene-linear Rec.709 and carries no profile.
    bool srgb = true;
    // EXIF for JPEG (a TIFF structure, without the "Exif\0\0" prefix); empty for none.
    std::vector<uint8_t> exif;
};

const char* formatExtension(FileFormat f);  // ".png", ".jpg", ".tif", ".exr"
// Save dialog filter, in FileFormat order.
extern const char* const kSaveImageFilter;
// From a file name's extension (.png, .jpg/.jpeg, .tif/.tiff, .exr); PNG for anything else.
FileFormat formatFromPath(const std::string& pathU8);
// The depth the format actually writes for a requested one (8/16 for PNG and TIFF, 16/32 for
// OpenEXR, 8 for JPEG).
int formatDepth(FileFormat f, int depth);

// Writes `img` as is: display formats clamp to 0..1 and quantise, OpenEXR keeps every value and
// stores colour premultiplied by alpha, as the format expects. Alpha is dropped when the image
// is fully opaque. Returns false and sets err on failure.
bool writeImage(const std::string& pathU8, const Image& img, const SaveOptions& opt, std::string& err);

// A compact ICC v2 display profile for sRGB IEC 61966-2.1 (D50 PCS, Bradford-adapted primaries,
// the sRGB tone curve as a 1024-entry table).
const std::vector<uint8_t>& srgbIccProfile();

// IEEE 754 half float, rounded to nearest even; out-of-range values clamp to +-65504.
uint16_t floatToHalf(float f);
