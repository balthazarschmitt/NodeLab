// The parallel JPEG decoder against stb_image: files from our own writer (restart intervals, 4:4:4
// and 4:2:0), stb_image_write's files (no restart markers), and damaged streams, which must either
// decode exactly as stb does or be left to it.
#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

#include "io/ImageWrite.h"
#include "io/JpegDecode.h"
#include "io/Paths.h"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

Image pattern(int w, int h) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img.pixel(size_t(y) * w + x);
            p[0] = float((x * 7 + y * 3) % 97) / 96.0f;
            p[1] = float(x) / float(w);
            p[2] = float((x ^ y) & 31) / 31.0f;
            p[3] = 1.0f;
        }
    return img;
}

Bytes ourJpeg(const Image& img, int quality) {
    const fs::path p = fs::temp_directory_path() / "nodelab_jpeg_decode.jpg";
    SaveOptions o;
    o.format = FileFormat::JPEG;
    o.jpegQuality = quality;
    std::string err;
    REQUIRE(writeImage(pathToU8(p), img, o, err));
    std::vector<char> f;
    REQUIRE(readFileBytes(pathToU8(p), f));
    fs::remove(p);
    return Bytes(f.begin(), f.end());
}

void appendBytes(void* ctx, void* data, int size) {
    auto* b = static_cast<Bytes*>(ctx);
    b->insert(b->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

Bytes stbJpeg(int w, int h, int comp, int quality) {
    std::vector<uint8_t> px(size_t(w) * h * comp);
    for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t((i * 2654435761u) >> 24 & 0x3f) + uint8_t(i % 160);
    Bytes out;
    REQUIRE(stbi_write_jpg_to_func(appendBytes, &out, w, h, comp, px.data(), quality));
    return out;
}

// Returns whether the parallel decoder took the file; when it did, every code must match stb's.
bool checkSame(const Bytes& file) {
    jpeg::Decoded d;
    const bool ours = jpeg::decode(file.data(), file.size(), d);
    int w = 0, h = 0, comp = 0;
    stbi_uc* ref = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &comp, 4);
    if (!ours) {
        stbi_image_free(ref);
        return false;
    }
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
    return true;
}

}  // namespace

TEST_CASE("JPEG decoder matches stb_image") {
    // Our writer: several restart intervals, 4:4:4 at high quality and 4:2:0 below.
    for (int quality : {95, 60})
        for (const auto& sz : {std::pair{640, 480}, std::pair{333, 517}, std::pair{1, 300}, std::pair{301, 1}}) {
            CAPTURE(quality);
            CAPTURE(sz.first);
            CAPTURE(sz.second);
            CHECK(checkSame(ourJpeg(pattern(sz.first, sz.second), quality)));
        }
    // stb_image_write: one interval, greyscale and colour.
    for (int comp : {1, 3})
        for (int quality : {95, 60}) {
            CAPTURE(comp);
            CHECK(checkSame(stbJpeg(123, 45, comp, quality)));
        }
}

TEST_CASE("JPEG decoder leaves damaged files to stb_image") {
    const Bytes good = ourJpeg(pattern(640, 480), 90);
    // Each restart marker's position.
    std::vector<size_t> rst;
    for (size_t i = 0; i + 1 < good.size(); ++i)
        if (good[i] == 0xff && good[i + 1] >= 0xd0 && good[i + 1] <= 0xd7) rst.push_back(i);
    REQUIRE(rst.size() >= 2);

    Bytes cut(good.begin(), good.begin() + long(good.size() / 2));
    checkSame(cut);  // decoded exactly, or left to stb
    Bytes noRst = good;
    noRst.erase(noRst.begin() + long(rst[0]), noRst.begin() + long(rst[0]) + 2);
    checkSame(noRst);
    for (size_t at : {good.size() / 3, good.size() / 2, rst[1] - 5}) {
        Bytes flipped = good;
        for (size_t i = at; i < at + 24 && i < flipped.size() - 2; ++i) flipped[i] ^= 0x5a;
        checkSame(flipped);
    }
    const Bytes junk = {0xff, 0xd8, 0xff, 0xd9};
    checkSame(junk);
    const Bytes empty = {0xff, 0xd8};
    jpeg::Decoded d;
    CHECK_FALSE(jpeg::decode(empty.data(), empty.size(), d));
}
