// WebP lossless export: a small VP8L decoder (only what the encoder writes: subtract green, the
// predictor modes it picks, the colour cache, LZ77 with plane codes 1-2 and plain distances)
// checks that every kind of image round-trips exactly. Pillow and browsers read these files too;
// this guards the encoder against regressions.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <vector>

#include "core/OutputSpace.h"
#include "io/ImageWrite.h"
#include "io/WebpEncode.h"

namespace fs = std::filesystem;

namespace {

struct Reader {
    const std::vector<uint8_t>& d;
    size_t bit = 0;
    bool bad = false;
    uint32_t get(int n) {
        uint32_t v = 0;
        for (int k = 0; k < n; ++k, ++bit) {
            if (bit / 8 >= d.size()) {
                bad = true;
                return 0;
            }
            v |= uint32_t((d[bit / 8] >> (bit % 8)) & 1) << k;
        }
        return v;
    }
};

// Canonical prefix code, decoded a bit at a time (as zlib's puff does).
struct Huff {
    std::vector<int> count = std::vector<int>(16, 0), symbols;
    int single = -1;
    bool build(const std::vector<int>& len) {
        int used = 0, last = 0;
        for (size_t s = 0; s < len.size(); ++s)
            if (len[s]) ++count[size_t(len[s])], ++used, last = int(s);
        if (used == 1) single = last;
        std::vector<int> offs(16, 0);
        for (int l = 1; l < 15; ++l) offs[size_t(l + 1)] = offs[size_t(l)] + count[size_t(l)];
        symbols.assign(size_t(used), 0);
        for (size_t s = 0; s < len.size(); ++s)
            if (len[s]) symbols[size_t(offs[size_t(len[s])]++)] = int(s);
        return used > 0;
    }
    int decode(Reader& r) const {
        if (single >= 0) return single;
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16; ++l) {
            code |= int(r.get(1));
            const int c = count[size_t(l)];
            if (code - c < first) return symbols[size_t(index + code - first)];
            index += c, first += c;
            first <<= 1, code <<= 1;
            if (r.bad) return -1;
        }
        r.bad = true;
        return -1;
    }
};

bool readCode(Reader& r, int size, Huff& h) {
    std::vector<int> len(size_t(size), 0);
    if (r.get(1)) {
        const int num = int(r.get(1)) + 1;
        const int s0 = int(r.get(r.get(1) ? 8 : 1));
        len[size_t(s0)] = 1;
        if (num == 2) len[r.get(8)] = 1;
        return h.build(len);
    }
    static const int order[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    std::vector<int> cl(19, 0);
    const int num = int(r.get(4)) + 4;
    for (int i = 0; i < num; ++i) cl[size_t(order[i])] = int(r.get(3));
    if (r.get(1)) return false;  // max_symbol: the encoder never writes it
    Huff ch;
    if (!ch.build(cl)) return false;
    int prev = 8;
    for (int i = 0; i < size && !r.bad;) {
        const int s = ch.decode(r);
        if (s < 0) return false;
        if (s < 16) {
            len[size_t(i++)] = s;
            if (s) prev = s;
            continue;
        }
        const int rep = s == 16 ? 3 + int(r.get(2)) : s == 17 ? 3 + int(r.get(3)) : 11 + int(r.get(7));
        const int v = s == 16 ? prev : 0;
        if (i + rep > size) return false;
        for (int k = 0; k < rep; ++k) len[size_t(i++)] = v;
    }
    return !r.bad && h.build(len);
}

bool decodeImage(Reader& r, int w, int h, bool main, std::vector<uint32_t>& out) {
    int cacheBits = 0;
    if (r.get(1)) cacheBits = int(r.get(4));
    if (main && r.get(1)) return false;  // meta prefix codes: not written
    const int sizes[5] = {256 + 24 + (cacheBits ? 1 << cacheBits : 0), 256, 256, 256, 40};
    Huff c[5];
    for (int k = 0; k < 5; ++k)
        if (!readCode(r, sizes[k], c[k])) return false;
    std::vector<uint32_t> cache(cacheBits ? size_t(1) << cacheBits : 0);
    const size_t n = size_t(w) * size_t(h);
    out.assign(n, 0);
    auto prefix = [&](int sym) {
        if (sym < 4) return sym + 1;
        const int eb = (sym - 2) >> 1, off = (2 + (sym & 1)) << eb;
        return off + int(r.get(eb)) + 1;
    };
    auto put = [&](size_t i, uint32_t v) {
        out[i] = v;
        if (cacheBits) cache[(0x1e35a7bdu * v) >> (32 - cacheBits)] = v;
    };
    for (size_t i = 0; i < n;) {
        const int g = c[0].decode(r);
        if (g < 0 || r.bad) return false;
        if (g < 256) {
            const int red = c[1].decode(r), blue = c[2].decode(r), a = c[3].decode(r);
            put(i++, uint32_t(a) << 24 | uint32_t(red) << 16 | uint32_t(g) << 8 | uint32_t(blue));
        } else if (g < 280) {
            const int length = prefix(g - 256);
            const int dcode = prefix(c[4].decode(r));
            const size_t dist = dcode == 1 ? size_t(w) : dcode == 2 ? 1 : dcode > 120 ? size_t(dcode - 120) : 0;
            if (!dist || dist > i || i + size_t(length) > n) return false;
            for (int k = 0; k < length; ++k, ++i) put(i, out[i - dist]);
        } else {
            put(i, cache[size_t(g - 280)]);
            ++i;
        }
    }
    return !r.bad;
}

int ch(uint32_t v, int s) { return int((v >> s) & 0xff); }

uint32_t predict(int mode, uint32_t l, uint32_t t, uint32_t tl) {
    uint32_t o = 0;
    switch (mode) {
        case 0: return 0xff000000u;
        case 1: return l;
        case 2: return t;
        case 7:
            for (int s = 0; s < 32; s += 8) o |= uint32_t((ch(l, s) + ch(t, s)) / 2) << s;
            return o;
        case 11: {
            int pl = 0, pt = 0;  // the spec's Select: distances of the gradient estimate
            for (int s = 0; s < 32; s += 8) {
                const int p = ch(l, s) + ch(t, s) - ch(tl, s);
                pl += std::abs(p - ch(l, s)), pt += std::abs(p - ch(t, s));
            }
            return pl < pt ? l : t;
        }
        case 12:
            for (int s = 0; s < 32; s += 8) o |= uint32_t(std::clamp(ch(l, s) + ch(t, s) - ch(tl, s), 0, 255)) << s;
            return o;
        default: return 0xdeadbeefu;
    }
}

uint32_t addPixels(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) r |= uint32_t((ch(a, s) + ch(b, s)) & 0xff) << s;
    return r;
}

// RGBA bytes from a VP8L bitstream, or empty when it can't be read.
std::vector<uint8_t> decodeVp8l(const std::vector<uint8_t>& d, int& w, int& h) {
    Reader r{d};
    if (r.get(8) != 0x2f) return {};
    w = int(r.get(14)) + 1, h = int(r.get(14)) + 1;
    r.get(1);
    if (r.get(3) != 0) return {};
    bool green = false;
    int predBits = 0;
    std::vector<uint32_t> modes;
    while (r.get(1)) {
        const uint32_t type = r.get(2);
        if (type == 2) {
            green = true;
        } else if (type == 0) {
            predBits = int(r.get(3)) + 2;
            const int bw = (w + (1 << predBits) - 1) >> predBits, bh = (h + (1 << predBits) - 1) >> predBits;
            if (!decodeImage(r, bw, bh, false, modes)) return {};
        } else {
            return {};
        }
    }
    std::vector<uint32_t> px;
    if (!decodeImage(r, w, h, true, px)) return {};
    if (predBits) {
        const int bw = (w + (1 << predBits) - 1) >> predBits;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                uint32_t pred;
                if (y == 0) pred = x == 0 ? 0xff000000u : px[i - 1];
                else if (x == 0) pred = px[i - size_t(w)];
                else {
                    const int mode = ch(modes[size_t(y >> predBits) * size_t(bw) + size_t(x >> predBits)], 8);
                    pred = predict(mode, px[i - 1], px[i - size_t(w)], px[i - size_t(w) - 1]);
                }
                px[i] = addPixels(px[i], pred);
            }
    }
    std::vector<uint8_t> rgba(px.size() * 4);
    for (size_t i = 0; i < px.size(); ++i) {
        const uint32_t v = px[i];
        const int g = ch(v, 8);
        rgba[i * 4] = uint8_t(ch(v, 16) + (green ? g : 0));
        rgba[i * 4 + 1] = uint8_t(g);
        rgba[i * 4 + 2] = uint8_t(ch(v, 0) + (green ? g : 0));
        rgba[i * 4 + 3] = uint8_t(ch(v, 24));
    }
    return rgba;
}

// The VP8L chunk's payload and the other chunk names in a .webp file.
std::vector<uint8_t> vp8lChunk(const std::vector<uint8_t>& f, std::vector<std::string>* names = nullptr,
                               std::vector<uint8_t>* iccp = nullptr) {
    if (f.size() < 12 || std::string(f.begin(), f.begin() + 4) != "RIFF" ||
        std::string(f.begin() + 8, f.begin() + 12) != "WEBP")
        return {};
    const uint32_t riff = uint32_t(f[4]) | uint32_t(f[5]) << 8 | uint32_t(f[6]) << 16 | uint32_t(f[7]) << 24;
    CHECK(riff + 8 == f.size());
    std::vector<uint8_t> out;
    for (size_t at = 12; at + 8 <= f.size();) {
        const std::string type(f.begin() + long(at), f.begin() + long(at) + 4);
        const size_t len = size_t(f[at + 4]) | size_t(f[at + 5]) << 8 | size_t(f[at + 6]) << 16 | size_t(f[at + 7]) << 24;
        if (names) names->push_back(type);
        const std::vector<uint8_t> data(f.begin() + long(at) + 8, f.begin() + long(at + 8 + len));
        if (type == "VP8L") out = data;
        if (type == "ICCP" && iccp) *iccp = data;
        at += 8 + len + (len & 1);
    }
    return out;
}

std::vector<uint8_t> roundTrip(const std::vector<uint8_t>& rgba, int w, int h) {
    const std::vector<uint8_t> enc = webp::encodeLossless(rgba.data(), w, h, true);
    int dw = 0, dh = 0;
    std::vector<uint8_t> dec = decodeVp8l(enc, dw, dh);
    CHECK(dw == w);
    CHECK(dh == h);
    return dec;
}

}  // namespace

TEST_CASE("WebP lossless round-trips every kind of image exactly") {
    std::mt19937 rng(7);
    struct Case {
        int w, h, kind;
    };
    // kind: 0 noise, 1 gradient, 2 flat, 3 few colours (cache and runs), 4 repeated tiles (LZ77)
    const Case cases[] = {{1, 1, 0}, {1, 37, 1}, {41, 1, 0}, {64, 64, 0}, {97, 53, 1}, {300, 200, 2},
                          {129, 70, 3}, {256, 300, 4}, {33, 33, 4}, {5000, 2, 2}};
    for (const Case& c : cases) {
        CAPTURE(c.w);
        CAPTURE(c.h);
        CAPTURE(c.kind);
        std::vector<uint8_t> rgba(size_t(c.w) * size_t(c.h) * 4);
        for (int y = 0; y < c.h; ++y)
            for (int x = 0; x < c.w; ++x) {
                uint8_t* p = &rgba[(size_t(y) * size_t(c.w) + size_t(x)) * 4];
                switch (c.kind) {
                    case 0:
                        for (int k = 0; k < 4; ++k) p[k] = uint8_t(rng());
                        break;
                    case 1: p[0] = uint8_t(x * 3), p[1] = uint8_t(y * 2 + x), p[2] = uint8_t(200 - y), p[3] = 255; break;
                    case 2: p[0] = 10, p[1] = 200, p[2] = 30, p[3] = 128; break;
                    case 3: {
                        const uint8_t v = uint8_t(((x / 7) ^ (y / 5)) % 6 * 40);
                        p[0] = v, p[1] = uint8_t(255 - v), p[2] = uint8_t(v / 2), p[3] = 255;
                        break;
                    }
                    default: {
                        const uint32_t s = uint32_t((x % 23) * 31 + (y % 17) * 7) * 2654435761u;
                        p[0] = uint8_t(s >> 24), p[1] = uint8_t(s >> 16), p[2] = uint8_t(s >> 8), p[3] = 255;
                    }
                }
            }
        CHECK(roundTrip(rgba, c.w, c.h) == rgba);
    }
}

TEST_CASE("WebP files: plain container for sRGB, VP8X with ICCP for wider spaces") {
    Image img(20, 10);
    for (size_t i = 0; i < img.pixelCount(); ++i) {
        float* p = img.pixel(i);
        p[0] = float(i % 20) / 19.0f, p[1] = 0.5f, p[2] = float(i / 20) / 9.0f, p[3] = i < 50 ? 0.5f : 1.0f;
    }
    const fs::path path = fs::temp_directory_path() / "refractory_test.webp";
    CHECK(formatFromPath(path.string()) == FileFormat::WEBP);
    CHECK(std::string(formatExtension(FileFormat::WEBP)) == ".webp");
    CHECK(formatDepth(FileFormat::WEBP, 16) == 8);
    for (int space : {int(outspace::sRGB), int(outspace::DisplayP3)}) {
        CAPTURE(space);
        SaveOptions opt;
        opt.format = FileFormat::WEBP;
        opt.space = space;
        std::string err;
        REQUIRE(writeImage(path.string(), img, opt, err));
        std::ifstream f(path, std::ios::binary);
        const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<std::string> names;
        std::vector<uint8_t> icc;
        const std::vector<uint8_t> vp8l = vp8lChunk(file, &names, &icc);
        if (space == outspace::sRGB) {
            CHECK(names == std::vector<std::string>{"VP8L"});
        } else {
            CHECK(names == std::vector<std::string>{"VP8X", "ICCP", "VP8L"});
            CHECK(icc == iccProfile(space));
        }
        int w = 0, h = 0;
        const std::vector<uint8_t> rgba = decodeVp8l(vp8l, w, h);
        REQUIRE(rgba.size() == 20 * 10 * 4);
        CHECK(rgba[0] == 0);
        CHECK(rgba[3] == 128);  // alpha kept
        CHECK(rgba[(19 + 9 * 20) * 4] == 255);
        CHECK(rgba[(19 + 9 * 20) * 4 + 3] == 255);
    }
}
