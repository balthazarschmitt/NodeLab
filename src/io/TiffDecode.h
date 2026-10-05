#pragma once
// A TIFF reader for photos and renders: baseline and tiled images of 8 or 16-bit integers or
// 16/32-bit floats, grey, RGB or palette, with alpha, uncompressed or LZW, Deflate or PackBits
// (with the horizontal and floating-point predictors). Reads the first image of the file.
// Everything is bounds-checked: damaged files fail with a reason (test_decode_fuzz.cpp).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tiffdec {

// Reads n bytes at offset `at`; false past the end of the file.
using Reader = std::function<bool(uint64_t at, void* dst, size_t n)>;

// What the first image's directory says, without decoding pixels.
struct Info {
    int w = 0, h = 0;
    bool isFloat = false;  // float samples: scene-linear data, like OpenEXR
    int orientation = 1;   // EXIF orientation (tag 274)
    std::vector<uint8_t> icc;
};

// True when the bytes start like a TIFF file ("II*\0" or "MM\0*").
bool isTiff(const uint8_t* data, size_t len);
bool probe(const Reader& read, Info& info, std::string& err);
// Probes a file on disk, reading only the header and the directory.
bool probeFile(const std::string& pathU8, Info& info, std::string& err);

struct Decoded {
    Info info;
    // RGBA, row by row: integers as codes 0..maxCode (alpha maxCode when opaque), floats as stored
    // (alpha 1 when opaque). Alpha stored premultiplied is divided out.
    int maxCode = 255;             // 255 or 65535 for integer images
    std::vector<uint16_t> codes;   // integer images
    std::vector<float> floats;     // float images
};
bool decode(const uint8_t* data, size_t len, Decoded& out, std::string& err);

}  // namespace tiffdec
