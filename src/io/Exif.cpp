#include "io/Exif.h"

#include <cstdint>
#include <fstream>
#include <vector>

#include "core/Parallel.h"
#include "io/Paths.h"

namespace exif {

namespace {

// Orientation from a TIFF block (the payload of the Exif APP1 segment after "Exif\0\0").
int tiffOrientation(const uint8_t* t, size_t n) {
    if (n < 8) return 1;
    const bool le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return 1;
    auto u16 = [&](size_t o) -> unsigned { return le ? t[o] | t[o + 1] << 8 : t[o] << 8 | t[o + 1]; };
    auto u32 = [&](size_t o) -> uint32_t {
        return le ? uint32_t(t[o]) | uint32_t(t[o + 1]) << 8 | uint32_t(t[o + 2]) << 16 | uint32_t(t[o + 3]) << 24
                  : uint32_t(t[o]) << 24 | uint32_t(t[o + 1]) << 16 | uint32_t(t[o + 2]) << 8 | uint32_t(t[o + 3]);
    };
    const size_t ifd = u32(4);
    if (ifd + 2 > n) return 1;
    const unsigned count = u16(ifd);
    for (unsigned i = 0; i < count; ++i) {
        const size_t e = ifd + 2 + size_t(i) * 12;
        if (e + 12 > n) break;
        if (u16(e) == 0x0112) {  // Orientation, SHORT
            const unsigned v = u16(e + 8);
            return v >= 1 && v <= 8 ? int(v) : 1;
        }
    }
    return 1;
}

}  // namespace

int jpegOrientation(const std::string& pathU8) {
    std::ifstream f(u8ToPath(pathU8), std::ios::binary);
    if (!f) return 1;
    uint8_t soi[2];
    if (!f.read(reinterpret_cast<char*>(soi), 2) || soi[0] != 0xFF || soi[1] != 0xD8) return 1;
    // Walk the segments before the image data; EXIF is in an APP1 near the start.
    for (int guard = 0; guard < 64; ++guard) {
        uint8_t h[4];
        if (!f.read(reinterpret_cast<char*>(h), 4) || h[0] != 0xFF) return 1;
        const uint8_t marker = h[1];
        const size_t len = size_t(h[2]) << 8 | h[3];
        if (marker == 0xDA || marker == 0xD9 || len < 2) return 1;  // start of scan: no EXIF
        std::vector<uint8_t> seg(len - 2);
        if (!f.read(reinterpret_cast<char*>(seg.data()), std::streamsize(seg.size()))) return 1;
        if (marker == 0xE1 && seg.size() > 6 && std::equal(seg.begin(), seg.begin() + 6, "Exif\0\0"))
            return tiffOrientation(seg.data() + 6, seg.size() - 6);
    }
    return 1;
}

std::shared_ptr<Image> applyOrientation(const std::shared_ptr<Image>& img, int o) {
    if (!img || o <= 1 || o > 8) return img;
    const int w = img->w, h = img->h;
    const bool swap = o >= 5;
    auto out = std::make_shared<Image>(swap ? h : w, swap ? w : h);
    const int ow = out->w, oh = out->h;
    // For each output pixel, the stored pixel it shows. Orientation values per the EXIF spec:
    // 2 mirror, 3 rotate 180, 4 flip, 5 transpose, 6 rotate 90 CW, 7 transverse, 8 rotate 90 CCW.
    parallelFor(oh, [&](int y) {
        for (int x = 0; x < ow; ++x) {
            int sx = x, sy = y;
            switch (o) {
                case 2: sx = w - 1 - x; break;
                case 3: sx = w - 1 - x, sy = h - 1 - y; break;
                case 4: sy = h - 1 - y; break;
                case 5: sx = y, sy = x; break;
                case 6: sx = y, sy = h - 1 - x; break;
                case 7: sx = w - 1 - y, sy = h - 1 - x; break;
                case 8: sx = w - 1 - y, sy = x; break;
            }
            const float* s = img->pixel(size_t(sy) * w + sx);
            float* d = out->pixel(size_t(y) * ow + x);
            for (int c = 0; c < 4; ++c) d[c] = s[c];
        }
    });
    return out;
}

}  // namespace exif
