// The zlib-ng PNG decoder against stb_image: every colour type and depth it takes, every filter type,
// IDAT split over several chunks, and the layouts it must leave to stb.
#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

#include <stb_image.h>
#include <zlib.h>

#include "io/PngDecode.h"

namespace {

using Bytes = std::vector<uint8_t>;

void put32(Bytes& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}

void chunk(Bytes& out, const char* type, const Bytes& data) {
    put32(out, uint32_t(data.size()));
    Bytes body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32(out, uint32_t(crc32(0, body.data(), uInt(body.size()))));
}

// A PNG with row y filtered by filter type y % 5 (or `onlyFilter`), from pseudo-random samples
// smoothed along the row so the predictors have something to predict.
struct PngSpec {
    int w = 7, h = 5, colorType = 2, depth = 8, interlace = 0;
    int idatChunks = 3;
    bool trns = false, palette = false;
    uint32_t seed = 1;
};

Bytes makePng(const PngSpec& s) {
    const int ch = s.palette ? 1 : s.colorType == 0 ? 1 : s.colorType == 2 ? 3 : s.colorType == 4 ? 2 : 4;
    const size_t bpp = size_t(ch) * size_t(s.depth / 8);
    const size_t rowBytes = size_t(s.w) * bpp;
    std::vector<Bytes> rows(size_t(s.h), Bytes(rowBytes));
    uint32_t r = s.seed;
    for (auto& row : rows)
        for (size_t i = 0; i < rowBytes; ++i) {
            r = r * 1664525u + 1013904223u;
            row[i] = uint8_t((i >= bpp ? row[i - bpp] : 128) + int(r >> 28) - 8);
        }
    Bytes filtered;
    for (int y = 0; y < s.h; ++y) {
        const int f = y % 5;
        filtered.push_back(uint8_t(f));
        const Bytes& cur = rows[size_t(y)];
        const Bytes zero(rowBytes, 0);
        const Bytes& prev = y ? rows[size_t(y - 1)] : zero;
        for (size_t i = 0; i < rowBytes; ++i) {
            const int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
            int pred = 0;
            if (f == 1) pred = a;
            if (f == 2) pred = b;
            if (f == 3) pred = (a + b) >> 1;
            if (f == 4) {
                const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
                pred = (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
            }
            filtered.push_back(uint8_t(cur[i] - pred));
        }
    }
    uLongf zn = compressBound(uLong(filtered.size()));
    Bytes z(zn);
    REQUIRE(compress2(z.data(), &zn, filtered.data(), uLong(filtered.size()), 6) == Z_OK);
    z.resize(zn);

    Bytes png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    Bytes ihdr;
    put32(ihdr, uint32_t(s.w));
    put32(ihdr, uint32_t(s.h));
    ihdr.push_back(uint8_t(s.depth));
    ihdr.push_back(uint8_t(s.palette ? 3 : s.colorType));
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(uint8_t(s.interlace));
    chunk(png, "IHDR", ihdr);
    if (s.palette) {
        Bytes plte;
        for (int i = 0; i < 256; ++i) plte.insert(plte.end(), {uint8_t(i), uint8_t(255 - i), uint8_t(i * 7)});
        chunk(png, "PLTE", plte);
    }
    if (s.trns) chunk(png, "tRNS", s.depth == 16 ? Bytes(6, 0) : Bytes{0, 0, 0, 0, 0, 0});
    chunk(png, "tEXt", Bytes{'a', 0, 'b'});  // an ancillary chunk to skip
    const size_t part = (z.size() + size_t(s.idatChunks) - 1) / size_t(s.idatChunks);
    for (size_t i = 0; i < z.size(); i += part)
        chunk(png, "IDAT", Bytes(z.begin() + long(i), z.begin() + long(std::min(z.size(), i + part))));
    chunk(png, "IEND", {});
    return png;
}

// Decodes with both and compares every RGBA code.
void checkSame(const Bytes& png) {
    png::Decoded d;
    REQUIRE(png::decode(png.data(), png.size(), d));
    int w = 0, h = 0, comp = 0;
    if (d.sixteen) {
        stbi_us* ref = stbi_load_16_from_memory(png.data(), int(png.size()), &w, &h, &comp, 4);
        REQUIRE(ref);
        REQUIRE(w == d.w);
        REQUIRE(h == d.h);
        std::vector<uint16_t> row(size_t(w) * 4);
        int bad = 0;
        for (int y = 0; y < h; ++y) {
            d.expandRow(y, row.data());
            bad += std::memcmp(row.data(), ref + size_t(y) * w * 4, row.size() * 2) != 0;
        }
        CHECK(bad == 0);
        stbi_image_free(ref);
    } else {
        stbi_uc* ref = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &comp, 4);
        REQUIRE(ref);
        REQUIRE(w == d.w);
        REQUIRE(h == d.h);
        std::vector<uint8_t> row(size_t(w) * 4);
        int bad = 0;
        for (int y = 0; y < h; ++y) {
            d.expandRow(y, row.data());
            bad += std::memcmp(row.data(), ref + size_t(y) * w * 4, row.size()) != 0;
        }
        CHECK(bad == 0);
        stbi_image_free(ref);
    }
}

}  // namespace

TEST_CASE("PNG decoder matches stb_image for every colour type, depth and filter") {
    const int sizes[][2] = {{1, 1}, {1, 6}, {7, 5}, {33, 11}, {300, 10}};
    for (int colorType : {0, 2, 4, 6})
        for (int depth : {8, 16})
            for (const auto& sz : sizes)
                for (int chunks : {1, 3}) {
                    CAPTURE(colorType);
                    CAPTURE(depth);
                    CAPTURE(sz[0]);
                    CAPTURE(sz[1]);
                    PngSpec s;
                    s.colorType = colorType;
                    s.depth = depth;
                    s.w = sz[0];
                    s.h = sz[1];
                    s.idatChunks = chunks;
                    s.seed = uint32_t(colorType * 100 + depth + sz[0]);
                    checkSame(makePng(s));
                }
}

TEST_CASE("PNG decoder leaves unusual and damaged files to stb_image") {
    png::Decoded d;
    PngSpec s;
    s.palette = true;
    s.depth = 8;
    Bytes png = makePng(s);
    CHECK_FALSE(png::decode(png.data(), png.size(), d));

    s = {};
    s.interlace = 1;
    png = makePng(s);
    CHECK_FALSE(png::decode(png.data(), png.size(), d));

    s = {};
    s.trns = true;
    png = makePng(s);
    CHECK_FALSE(png::decode(png.data(), png.size(), d));

    // Truncated in the image data or the header, and not a whole signature.
    s = {};
    s.w = 50;
    s.h = 40;
    png = makePng(s);
    REQUIRE(png::decode(png.data(), png.size(), d));
    Bytes cut(png.begin(), png.begin() + long(png.size() / 2));
    CHECK_FALSE(png::decode(cut.data(), cut.size(), d));
    CHECK_FALSE(png::decode(png.data(), 20, d));
    const Bytes sig = {0x89, 'P', 'N', 'G'};
    CHECK_FALSE(png::decode(sig.data(), sig.size(), d));
}
