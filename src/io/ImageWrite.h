#pragma once
// Export file writers: PNG and TIFF (8 or 16 bit), JPEG, OpenEXR (half or full float), WebP
// (8 bit, lossless or lossy), JPEG XL (8 or 16 bit) and AVIF (8 or 10 bit).
#include <cstdint>
#include <string>
#include <vector>

#include "core/Image.h"

// Export formats, in the order of the Export window's and the File Output node's Format menus.
enum class FileFormat { PNG = 0, JPEG = 1, TIFF = 2, EXR = 3, WEBP = 4, JXL = 5, AVIF = 6 };
inline constexpr int kFileFormatCount = 7;
// Formats with a quality setting (and, but for JPEG, a lossless mode).
inline bool hasQuality(FileFormat f) {
    return f == FileFormat::JPEG || f == FileFormat::WEBP || f == FileFormat::JXL || f == FileFormat::AVIF;
}
inline bool hasLossless(FileFormat f) { return f == FileFormat::WEBP || f == FileFormat::JXL || f == FileFormat::AVIF; }
// Formats that carry Rec.2100 PQ (HDR): PNG, JPEG XL and AVIF.
inline bool hasHdr(FileFormat f) { return f == FileFormat::PNG || f == FileFormat::JXL || f == FileFormat::AVIF; }

struct SaveOptions {
    FileFormat format = FileFormat::PNG;
    // Bits per channel. PNG, TIFF and JPEG XL: 8 or 16. AVIF: 8, or 10 when asked for 16.
    // OpenEXR: 16 (half float) or 32 (full float). JPEG and WebP are always 8.
    int depth = 8;
    // Quality 1..100 for JPEG and the lossy modes of WebP, JPEG XL and AVIF.
    int jpegQuality = 95;
    // WebP, JPEG XL and AVIF: keep every value exactly instead.
    bool lossless = true;
    // Tag display formats with their colour space: the sRGB chunk in PNG (an ICC profile for
    // other spaces, the cICP chunk for HDR), an ICC profile in JPEG and TIFF, an ICC profile or
    // CICP in WebP, JPEG XL and AVIF. OpenEXR is always scene-linear Rec.709 and carries no profile.
    bool srgb = true;
    // The values' colour space (outspace::Space); the caller has converted them to it.
    int space = 0;
    // EXIF for JPEG, WebP, JPEG XL and AVIF (a TIFF structure, without the "Exif\0\0" prefix); empty for none.
    std::vector<uint8_t> exif;
    // An XMP packet (library::xmpPacket: title, caption, keywords, rating, label) for PNG (iTXt),
    // JPEG (APP1), TIFF (tag 700), WebP (its XMP chunk), JPEG XL ("xml " box) and AVIF; empty for
    // none. OpenEXR has no place for it that other programs read.
    std::string xmp;
};

const char* formatExtension(FileFormat f);  // ".png", ".jpg", ".tif", ".exr", ".webp", ".jxl", ".avif"
// Save dialog filter, in FileFormat order.
extern const char* const kSaveImageFilter;
// From a file name's extension (.png, .jpg/.jpeg, .tif/.tiff, .exr, .webp, .jxl, .avif); PNG for
// anything else.
FileFormat formatFromPath(const std::string& pathU8);
// The depth the format actually writes for a requested one (8/16 for PNG, TIFF and JPEG XL; for
// AVIF 8/16, meaning 8 or 10 bits; 16/32 for OpenEXR; 8 for JPEG and WebP).
int formatDepth(FileFormat f, int depth);

// Writes `img` as is: display formats clamp to 0..1 and quantise, OpenEXR keeps every value and
// stores colour premultiplied by alpha, as the format expects. Alpha is dropped when the image
// is fully opaque. Returns false and sets err on failure.
bool writeImage(const std::string& pathU8, const Image& img, const SaveOptions& opt, std::string& err);

// A compact ICC v2 display profile for sRGB IEC 61966-2.1 (D50 PCS, Bradford-adapted primaries,
// the sRGB tone curve as a 1024-entry table).
const std::vector<uint8_t>& srgbIccProfile();
// The same for any export space (outspace::Space; Rec.2100 PQ gets Rec.2020's).
const std::vector<uint8_t>& iccProfile(int space);

// IEEE 754 half float, rounded to nearest even; out-of-range values clamp to +-65504.
uint16_t floatToHalf(float f);
