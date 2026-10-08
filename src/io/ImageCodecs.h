#pragma once
// The formats Refractory reads and writes through libraries: lossy WebP (libwebp; lossless WebP is
// Refractory's own WebpEncode), JPEG XL (libjxl) and AVIF (libavif with libaom). ImageWrite and
// ImageIO call these; everything about colour tagging and metadata placement is decided here.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "io/WebpEncode.h"

namespace codecs {

using Bytes = std::vector<uint8_t>;

// Quantised pixels to encode: `comp` (3 or 4) interleaved channels per pixel, rows top to bottom,
// 8-bit or 16-bit (full range 0..65535) samples.
struct Pixels {
    const void* data = nullptr;
    int w = 0, h = 0, comp = 3;
    bool sixteen = false;
    int bits = 16;  // of 16-bit samples: 16, or 10 for AVIF (0..1023), so they're rounded once
};

// How the pixels are tagged and what rides along.
struct Tags {
    Bytes icc;         // the profile of their space; empty for sRGB
    bool pq = false;   // Rec.2100 PQ (HDR): tagged with CICP / the PQ colour encoding, not icc
    Bytes exif;        // a TIFF structure (no "Exif\0\0" prefix); empty for none
    std::string xmp;   // an XMP packet; empty for none
};

// libwebp's lossy picture at quality 1..100, as the image chunks for webp::container: "VP8 ",
// after "ALPH" when there's alpha (comp 4). 8-bit pixels only.
bool webpLossy(const Pixels& px, int quality, std::vector<webp::Chunk>& chunks, std::string& err);

// A whole .jxl file: lossless (modular, the exact samples), or VarDCT at quality 1..99
// (libjxl's distance for that quality, as cjxl -q). 8-bit or 16-bit samples.
bool encodeJxl(const Pixels& px, int quality, bool lossless, const Tags& tags, Bytes& out, std::string& err);

// A whole .avif file: 8-bit pixels as 8-bit AV1, 16-bit ones as 10-bit. Lossless stores RGB
// (identity matrix) at 4:4:4; quality 90 and above keeps 4:4:4 chroma, below it 4:2:0.
bool encodeAvif(const Pixels& px, int quality, bool lossless, const Tags& tags, Bytes& out, std::string& err);

enum class Kind { None, WebP, JXL, AVIF };
// From the first bytes of a file (12 are enough).
Kind sniff(const uint8_t* data, size_t n);

struct Decoded {
    int w = 0, h = 0;
    std::vector<uint16_t> rgba;  // full range 0..65535, 4 channels, as stored (not yet oriented)
    int orientation = 1;         // EXIF orientation the file asks for (JPEG XL, AVIF's irot/imir)
};
bool decode(const uint8_t* data, size_t n, Decoded& out, std::string& err);

// The ICC profile describing a file's samples: embedded, or built for the colour space its CICP
// or JPEG XL colour encoding names. Empty for sRGB and for anything unreadable.
Bytes profile(const uint8_t* data, size_t n);

}  // namespace codecs
