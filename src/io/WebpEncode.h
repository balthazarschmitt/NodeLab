#pragma once
// A WebP lossless (VP8L) encoder, so exports can be WebP without linking libwebp. It uses the
// subtract-green and predictor transforms, LZ77 backward references, a colour cache and
// length-limited canonical prefix codes. It has no meta prefix codes or cross-colour transform,
// so files are somewhat larger than cwebp's, but any WebP decoder reads them.
#include <cstdint>
#include <string>
#include <vector>

namespace webp {

// The VP8L bitstream (the payload of a VP8L chunk) for 8-bit RGBA pixels, rows top to bottom.
// alphaUsed is the header's hint; the alpha bytes are always stored. Width and height are
// 1..16384.
std::vector<uint8_t> encodeLossless(const uint8_t* rgba, int w, int h, bool alphaUsed);

// A whole .webp file: the VP8L chunk, inside an extended (VP8X) container when there is an ICC
// profile, EXIF (a TIFF structure, as JPEG's APP1 holds after "Exif\0\0") or an XMP packet.
std::vector<uint8_t> container(const std::vector<uint8_t>& vp8l, int w, int h, bool alphaUsed,
                               const std::vector<uint8_t>& icc, const std::vector<uint8_t>& exif,
                               const std::vector<uint8_t>& xmp = {});

// The same around any image chunks, in order: {"VP8L"}, or a lossy picture's {"ALPH", "VP8 "}.
struct Chunk {
    std::string type;  // four characters
    std::vector<uint8_t> data;
};
std::vector<uint8_t> container(const std::vector<Chunk>& image, int w, int h, bool alphaUsed,
                               const std::vector<uint8_t>& icc, const std::vector<uint8_t>& exif,
                               const std::vector<uint8_t>& xmp);

}  // namespace webp
