// Export writers (16-bit PNG, TIFF, OpenEXR, JPEG metadata), the sRGB ICC profile, EXIF for
// exports, and the Lanczos export resize.
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>
#include <zlib.h>

#include "core/ColorMath.h"
#include "graph/Graph.h"
#include "io/Exif.h"
#include "io/Export.h"
#include "io/ImageIO.h"
#include "io/ImageWrite.h"
#include "io/Library.h"
#include "io/Paths.h"
#include "io/Tiff.h"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

fs::path tempDir(const char* name) {
    const fs::path d = fs::temp_directory_path() / name;
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

Bytes readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool contains(const Bytes& b, const std::string& s) {
    return std::search(b.begin(), b.end(), s.begin(), s.end()) != b.end();
}

uint32_t le32(const Bytes& b, size_t o) { return b[o] | b[o + 1] << 8 | b[o + 2] << 16 | uint32_t(b[o + 3]) << 24; }
unsigned le16(const Bytes& b, size_t o) { return b[o] | b[o + 1] << 8; }

Bytes inflate(const uint8_t* z, size_t n, size_t outSize) {
    Bytes out(outSize);
    const int got = stbi_zlib_decode_buffer(reinterpret_cast<char*>(out.data()), int(out.size()),
                                            reinterpret_cast<const char*>(z), int(n));
    REQUIRE(got == int(outSize));
    return out;
}

float halfToFloat(uint16_t h) {
    const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
    const float s = h & 0x8000 ? -1.0f : 1.0f;
    if (e == 0) return s * std::ldexp(float(m), -24);
    if (e == 31) return m ? NAN : s * INFINITY;
    return s * std::ldexp(float(m | 0x400), e - 25);
}

// A gradient with values that show up quantisation: x across red, y across green.
Image gradient(int w, int h, float alpha = 1.0f) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img.pixel(size_t(y) * w + x);
            p[0] = float(x) / (w - 1), p[1] = float(y) / (h - 1), p[2] = 0.3f, p[3] = alpha;
        }
    return img;
}

// Exact 8-bit values (k / 255), so the writers' quantising gives back k: noise over gradients,
// like a photo, or (`pattern`) a texture that repeats every 23 rows and 37 columns, like graphics.
Image bytesImage(int w, int h, bool pattern, std::vector<uint8_t>* rgb = nullptr) {
    Image img(w, h);
    uint32_t seed = 7;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img.pixel(size_t(y) * w + x);
            for (int c = 0; c < 3; ++c) {
                int v;
                if (pattern) {
                    v = int((uint32_t((x % 37) * 7919 + (y % 23) * 104729 + c * 31) * 2654435761u) >> 24);
                } else {
                    seed = seed * 1664525u + 1013904223u;
                    v = std::clamp(int(x * 200 / w + y * 40 / h + c * 10) + int(seed >> 28) - 8, 0, 255);
                }
                p[c] = v / 255.0f;
                if (rgb) rgb->push_back(uint8_t(v));
            }
            p[3] = 1.0f;
        }
    return img;
}

// Whether every chunk of a PNG file is whole and its CRC holds.
bool pngChunksValid(const Bytes& b) {
    size_t pos = 8;
    while (pos + 12 <= b.size()) {
        const size_t len = size_t(b[pos]) << 24 | b[pos + 1] << 16 | b[pos + 2] << 8 | b[pos + 3];
        if (pos + 12 + len > b.size()) return false;
        const uint32_t crc = uint32_t(b[pos + 8 + len]) << 24 | b[pos + 9 + len] << 16 | b[pos + 10 + len] << 8 | b[pos + 11 + len];
        if (crc32(0, b.data() + pos + 4, uInt(len + 4)) != crc) return false;
        if (std::memcmp(&b[pos + 4], "IEND", 4) == 0) return pos + 12 == b.size();
        pos += 12 + len;
    }
    return false;
}

}  // namespace

TEST_CASE("floatToHalf rounds like IEEE half") {
    CHECK(floatToHalf(0.0f) == 0x0000);
    CHECK(floatToHalf(-0.0f) == 0x8000);
    CHECK(floatToHalf(1.0f) == 0x3C00);
    CHECK(floatToHalf(0.5f) == 0x3800);
    CHECK(floatToHalf(-2.0f) == 0xC000);
    CHECK(floatToHalf(0.1f) == 0x2E66);
    CHECK(floatToHalf(65504.0f) == 0x7BFF);
    CHECK(floatToHalf(1e9f) == 0x7BFF);  // clamped, not inf
    CHECK(floatToHalf(std::ldexp(1.0f, -24)) == 0x0001);  // smallest subnormal
    CHECK(floatToHalf(std::ldexp(1.0f, -14)) == 0x0400);  // smallest normal
    CHECK((floatToHalf(NAN) & 0x7C00) == 0x7C00);
    for (float v : {0.001f, 0.18f, 0.7f, 3.3f, 1234.5f}) CHECK(halfToFloat(floatToHalf(v)) == doctest::Approx(v).epsilon(0.001));
}

TEST_CASE("TIFF directory entries keep their counts and values") {
    tiff::Ifd ifd;
    ifd.ascii(0x010F, "Canon");     // 6 bytes: out of line
    ifd.shorts(0x0112, {6});        // inline
    ifd.longs(0x0111, {1, 2, 3});  // out of line
    Bytes t = tiff::header();
    const uint32_t at = ifd.write(t);
    REQUIRE(le16(t, at) == 3);
    // Sorted by tag: 0x010F, 0x0111, 0x0112.
    CHECK(le16(t, at + 2) == 0x010F);
    CHECK(le32(t, at + 2 + 4) == 6);
    CHECK(std::string(reinterpret_cast<const char*>(&t[le32(t, at + 2 + 8)])) == "Canon");
    CHECK(le16(t, at + 14) == 0x0111);
    CHECK(le32(t, at + 14 + 4) == 3);
    CHECK(le32(t, le32(t, at + 14 + 8) + 8) == 3);
    CHECK(le16(t, at + 26) == 0x0112);
    CHECK(le16(t, at + 26 + 8) == 6);
}

TEST_CASE("sRGB ICC profile is well formed") {
    const Bytes& p = srgbIccProfile();
    REQUIRE(p.size() > 128);
    const uint32_t size = uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
    CHECK(size == p.size());
    CHECK(std::memcmp(&p[12], "mntr", 4) == 0);
    CHECK(std::memcmp(&p[16], "RGB ", 4) == 0);
    CHECK(std::memcmp(&p[36], "acsp", 4) == 0);
    const uint32_t tags = uint32_t(p[128]) << 24 | p[129] << 16 | p[130] << 8 | p[131];
    CHECK(tags == 9);
    for (uint32_t i = 0; i < tags; ++i) {
        const size_t e = 132 + i * 12;
        const uint32_t off = uint32_t(p[e + 4]) << 24 | p[e + 5] << 16 | p[e + 6] << 8 | p[e + 7];
        const uint32_t len = uint32_t(p[e + 8]) << 24 | p[e + 9] << 16 | p[e + 10] << 8 | p[e + 11];
        CHECK(off % 4 == 0);
        CHECK(off + len <= p.size());
    }
}

TEST_CASE("PNG export: 8 and 16 bit, tagged sRGB") {
    const fs::path dir = tempDir("nodelab_png16");
    const Image img = gradient(300, 7);
    std::string err;
    SaveOptions o;
    o.depth = 16;
    const std::string p16 = pathToU8(dir / "a.png");
    REQUIRE(writeImage(p16, img, o, err));
    const Bytes b = readAll(dir / "a.png");
    CHECK(b[24] == 16);  // IHDR bit depth
    CHECK(contains(b, "sRGB"));
    int w, h, n;
    stbi_us* px = stbi_load_16(p16.c_str(), &w, &h, &n, 0);
    REQUIRE(px);
    CHECK(w == 300);
    CHECK(h == 7);
    CHECK(n == 3);  // opaque: no alpha channel
    int maxErr = 0;
    for (int i = 0; i < w * h; ++i)
        for (int c = 0; c < 3; ++c)
            maxErr = std::max(maxErr, std::abs(int(px[i * 3 + c]) - int(std::lround(img.px[size_t(i) * 4 + c] * 65535.0f))));
    stbi_image_free(px);
    CHECK(maxErr == 0);

    o.depth = 8;
    const std::string p8 = pathToU8(dir / "b.png");
    REQUIRE(writeImage(p8, gradient(20, 4, 0.5f), o, err));
    const Bytes b8 = readAll(dir / "b.png");
    CHECK(b8[24] == 8);
    CHECK(b8[25] == 6);  // RGBA: the alpha is kept
    CHECK(contains(b8, "sRGB"));
    auto back = loadImage(p8, err);
    REQUIRE(back);
    CHECK(back->pixel(19)[0] == doctest::Approx(1.0f));
    CHECK(back->pixel(19)[3] == doctest::Approx(128 / 255.0f));
    fs::remove_all(dir);
}

TEST_CASE("TIFF export: Deflate strips with the predictor decode to the pixels") {
    const fs::path dir = tempDir("nodelab_tiff");
    for (int depth : {8, 16}) {
        const Image img = gradient(257, 600, 0.75f);
        SaveOptions o;
        o.format = FileFormat::TIFF;
        o.depth = depth;
        std::string err;
        const fs::path path = dir / ("t" + std::to_string(depth) + ".tif");
        REQUIRE(writeImage(pathToU8(path), img, o, err));
        const Bytes b = readAll(path);
        REQUIRE(b.size() > 8);
        CHECK(b[0] == 'I');
        CHECK(le16(b, 2) == 42);
        // Read the IFD.
        const size_t ifd = le32(b, 4);
        std::map<unsigned, std::pair<unsigned, size_t>> tags;  // tag -> (count, value offset)
        for (unsigned i = 0, n = le16(b, ifd); i < n; ++i) {
            const size_t e = ifd + 2 + i * 12;
            tags[le16(b, e)] = {le32(b, e + 4), e + 8};
        }
        auto value = [&](unsigned tag, unsigned idx = 0) -> uint32_t {
            const auto [count, at] = tags.at(tag);
            const unsigned type = le16(b, at - 6);
            const size_t sz = type == 3 ? 2 : 4;
            const size_t base = count * sz > 4 ? le32(b, at) : at;
            return sz == 2 ? le16(b, base + idx * 2) : le32(b, base + idx * 4);
        };
        CHECK(value(256) == 257);
        CHECK(value(257) == 600);
        CHECK(value(258) == unsigned(depth));
        CHECK(value(259) == 8);
        CHECK(value(277) == 4);
        CHECK(value(338) == 2);
        CHECK(value(317) == 2);
        REQUIRE(tags.count(34675));
        CHECK(tags.at(34675).first == srgbIccProfile().size());

        const unsigned rps = value(278), strips = tags.at(273).first;
        CHECK(strips == (600 + rps - 1) / rps);
        CHECK(strips > 1);  // several strips, compressed in parallel
        const size_t bps = depth / 8, rowBytes = 257 * 4 * bps;
        int maxErr = 0;
        for (unsigned s = 0; s < strips; ++s) {
            const unsigned rows = std::min(rps, 600 - s * rps);
            Bytes raw = inflate(&b[value(273, s)], value(279, s), rows * rowBytes);
            for (unsigned r = 0; r < rows; ++r) {
                std::vector<int> prev(4, 0);
                for (int x = 0; x < 257; ++x)
                    for (int c = 0; c < 4; ++c) {
                        const size_t at = r * rowBytes + (size_t(x) * 4 + c) * bps;
                        const int d = depth == 16 ? int(le16(raw, at)) : int(raw[at]);
                        const int v = (prev[c] + d) & (depth == 16 ? 0xFFFF : 0xFF);
                        prev[c] = v;
                        const float want = img.pixel(size_t(s * rps + r) * 257 + x)[c];
                        maxErr = std::max(maxErr, std::abs(v - int(std::lround(want * (depth == 16 ? 65535 : 255)))));
                    }
            }
        }
        CHECK(maxErr == 0);
    }
    fs::remove_all(dir);
}

TEST_CASE("OpenEXR export: ZIP blocks decode to the linear values") {
    const fs::path dir = tempDir("nodelab_exr");
    Image img = gradient(70, 37, 0.5f);
    img.pixel(5)[0] = 7.25f;   // above 1 survives
    img.pixel(6)[1] = -0.5f;   // so do negatives
    for (int depth : {16, 32}) {
        SaveOptions o;
        o.format = FileFormat::EXR;
        o.depth = depth;
        std::string err;
        const fs::path path = dir / ("e" + std::to_string(depth) + ".exr");
        REQUIRE(writeImage(pathToU8(path), img, o, err));
        const Bytes b = readAll(path);
        CHECK(le32(b, 0) == 20000630);  // magic
        CHECK(contains(b, "channels"));
        // Skip the header: attributes until an empty name.
        size_t at = 8;
        while (b[at] != 0) {
            while (b[at]) ++at;  // name
            ++at;
            while (b[at]) ++at;  // type
            ++at;
            at += 4 + le32(b, at);
        }
        ++at;
        const int blocks = (37 + 15) / 16;
        const size_t sample = depth / 8;
        float maxErr = 0;
        for (int k = 0; k < blocks; ++k) {
            const size_t off = size_t(le32(b, at + k * 8));
            const int y0 = int(le32(b, off));
            CHECK(y0 == k * 16);
            const uint32_t size = le32(b, off + 4);
            const int lines = std::min(16, 37 - y0);
            const size_t rawSize = size_t(lines) * 70 * 4 * sample;
            Bytes raw;
            if (size < rawSize) {
                Bytes t = inflate(&b[off + 8], size, rawSize);
                for (size_t i = 1; i < t.size(); ++i) t[i] = uint8_t(int(t[i - 1]) + int(t[i]) - 128);
                raw.resize(rawSize);
                const size_t half = (rawSize + 1) / 2;
                for (size_t i = 0; i < rawSize; ++i) raw[i] = t[(i & 1) ? half + i / 2 : i / 2];
            } else {
                raw.assign(b.begin() + off + 8, b.begin() + off + 8 + rawSize);
            }
            for (int l = 0; l < lines; ++l)
                for (int ch = 0; ch < 4; ++ch) {  // A, B, G, R
                    const int ci = ch == 0 ? 3 : 3 - ch;
                    for (int x = 0; x < 70; ++x) {
                        const size_t p = ((size_t(l) * 4 + ch) * 70 + x) * sample;
                        float v;
                        if (depth == 16) v = halfToFloat(uint16_t(le16(raw, p)));
                        else std::memcpy(&v, &raw[p], 4);
                        const float* src = img.pixel(size_t(y0 + l) * 70 + x);
                        const float want = ci == 3 ? src[3] : src[ci] * src[3];  // premultiplied
                        maxErr = std::max(maxErr, std::abs(v - want) / std::max(1.0f, std::abs(want)));
                    }
                }
        }
        CHECK(maxErr < (depth == 16 ? 1e-3f : 1e-7f));
    }
    fs::remove_all(dir);
}

TEST_CASE("PNG and TIFF export: photos and graphics both compress, chunks valid") {
    // Several 1 MB zlib segments; photos take run-length matching, graphics the LZ matcher.
    const fs::path dir = tempDir("nodelab_pngzip");
    std::string err;
    for (bool pattern : {false, true}) {
        const Image img = bytesImage(300, 2000, pattern);
        for (FileFormat f : {FileFormat::PNG, FileFormat::TIFF}) {
            SaveOptions o;
            o.format = f;
            const std::string path = pathToU8(dir / (f == FileFormat::PNG ? "a.png" : "a.tif"));
            REQUIRE(writeImage(path, img, o, err));
            const Bytes b = readAll(path);
            if (f == FileFormat::PNG) CHECK(pngChunksValid(b));
            // Graphics compress to a fraction of their size; noisy photos less so.
            if (pattern) CHECK(b.size() < img.pixelCount() * 3 / 10);
            if (f == FileFormat::TIFF) continue;  // the TIFF test above decodes strips
            auto back = loadImage(path, err);
            REQUIRE(back);
            int wrong = 0;
            for (size_t i = 0; i < img.pixelCount(); ++i)
                for (int c = 0; c < 3; ++c) wrong += std::lround(back->pixel(i)[c] * 255.0f) != std::lround(img.pixel(i)[c] * 255.0f);
            CHECK(wrong == 0);
        }
    }
    fs::remove_all(dir);
}

TEST_CASE("JPEG export in parallel strips decodes like a single stb encode") {
    const fs::path dir = tempDir("nodelab_jpegstrips");
    std::string err;
    std::vector<uint8_t> rgb;
    const Image img = bytesImage(203, 150, false, &rgb);  // not whole MCUs either way
    for (int quality : {95, 80}) {                        // 4:4:4 and 4:2:0 chroma
        SaveOptions o;
        o.format = FileFormat::JPEG;
        o.jpegQuality = quality;
        const std::string path = pathToU8(dir / "a.jpg");
        REQUIRE(writeImage(path, img, o, err));
        const Bytes b = readAll(path);
        const Bytes dri = {0xFF, 0xDD};  // a restart interval: it was split
        CHECK(std::search(b.begin(), b.end(), dri.begin(), dri.end()) != b.end());
        Bytes single;
        REQUIRE(stbi_write_jpg_to_func([](void* ctx, void* d, int n) {
            auto* out = static_cast<Bytes*>(ctx);
            out->insert(out->end(), static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + n);
        }, &single, img.w, img.h, 3, rgb.data(), quality));
        int w1, h1, n1, w2, h2, n2;
        stbi_uc* a = stbi_load_from_memory(b.data(), int(b.size()), &w1, &h1, &n1, 3);
        stbi_uc* s = stbi_load_from_memory(single.data(), int(single.size()), &w2, &h2, &n2, 3);
        REQUIRE(a);
        REQUIRE(s);
        CHECK(w1 == img.w);
        CHECK(h1 == img.h);
        CHECK(std::memcmp(a, s, size_t(img.w) * img.h * 3) == 0);
        stbi_image_free(a);
        stbi_image_free(s);
    }
    fs::remove_all(dir);
}

TEST_CASE("JPEG export carries the ICC profile and EXIF") {
    const fs::path dir = tempDir("nodelab_jpegmeta");
    SaveOptions o;
    o.format = FileFormat::JPEG;
    tiff::Ifd ifd;
    ifd.ascii(0x010F, "TestCam");
    o.exif = tiff::header();
    tiff::set32(o.exif, 4, ifd.write(o.exif));
    std::string err;
    const std::string p = pathToU8(dir / "a.jpg");
    REQUIRE(writeImage(p, gradient(16, 16), o, err));
    const Bytes b = readAll(dir / "a.jpg");
    CHECK(b[0] == 0xFF);
    CHECK(b[1] == 0xD8);
    CHECK(contains(b, std::string("ICC_PROFILE\0", 12)));
    CHECK(contains(b, std::string("Exif\0\0II", 8)));
    CHECK(contains(b, "TestCam"));
    auto back = loadImage(p, err);
    REQUIRE(back);
    CHECK(back->w == 16);
    fs::remove_all(dir);
}

TEST_CASE("EXIF for exports: upright, resized, no stale thumbnail") {
    const fs::path dir = tempDir("nodelab_exifexport");
    // A source JPEG whose EXIF says "rotate 90", with an Exif IFD (pixel size) and an IFD1 link.
    std::string err;
    SaveOptions o;
    o.format = FileFormat::JPEG;
    tiff::Ifd ifd0, ex, ifd1;
    ifd0.ascii(0x010F, "TestCam");
    ifd0.shorts(0x0112, {6});
    ex.longs(0xA002, {4000});
    ex.longs(0xA003, {3000});
    ifd1.longs(0x0201, {0});
    Bytes t = tiff::header();
    const uint32_t exOff = ex.write(t);
    const uint32_t ifd1Off = ifd1.write(t);
    ifd0.longs(0x8769, {exOff});
    const uint32_t ifd0Off = ifd0.write(t);
    tiff::set32(t, 4, ifd0Off);
    tiff::set32(t, ifd0Off + 2 + 3 * 12, ifd1Off);  // link IFD1 after IFD0's 3 entries
    o.exif = t;
    const std::string src = pathToU8(dir / "src.jpg");
    REQUIRE(writeImage(src, gradient(8, 8), o, err));
    CHECK(exif::jpegOrientation(src) == 6);

    const Bytes e = exif::exportBlock(src, 600, 800);
    REQUIRE(e.size() == t.size());
    CHECK(le32(e, ifd0Off + 2 + 3 * 12) == 0);  // IFD1 unlinked
    auto entryValue = [&](size_t ifd, unsigned tag) -> uint32_t {
        for (unsigned i = 0, n = le16(e, ifd); i < n; ++i)
            if (le16(e, ifd + 2 + i * 12) == tag) return le32(e, ifd + 2 + i * 12 + 8);
        return 0xFFFFFFFF;
    };
    CHECK((entryValue(ifd0Off, 0x0112) & 0xFFFF) == 1);
    CHECK(entryValue(exOff, 0xA002) == 600);
    CHECK(entryValue(exOff, 0xA003) == 800);

    // Formats without EXIF give none.
    CHECK(exif::exportBlock(pathToU8(dir / "nothing.png"), 1, 1).empty());
    CHECK(exif::exportBlock("", 1, 1).empty());
    fs::remove_all(dir);
}

TEST_CASE("Lanczos export resize works in linear light without halos") {
    // A 1-pixel checkerboard averages to 0.5 in linear light.
    Image check(64, 64);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            float* p = check.pixel(size_t(y) * 64 + x);
            p[0] = p[1] = p[2] = float((x + y) & 1), p[3] = 1;
        }
    auto lin = resizeLanczos(check, 16, 16);
    CHECK(lin->pixel(5 * 16 + 5)[0] == doctest::Approx(0.5f).epsilon(0.01));
    // Legacy (sRGB-encoded) values are averaged as light too: 0.5 linear encodes to ~0.735.
    auto enc = resizeLanczos(check, 16, 16, true);
    CHECK(enc->pixel(5 * 16 + 5)[0] == doctest::Approx(colormath::linearToSrgb(0.5f)).epsilon(0.01));

    // A hard edge: no overshoot either side, and flat areas stay exact.
    Image step(100, 10);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 100; ++x) {
            float* p = step.pixel(size_t(y) * 100 + x);
            p[0] = p[1] = p[2] = x < 50 ? 0.05f : 4.0f, p[3] = 1;
        }
    auto r = resizeLanczos(step, 33, 3);
    float lo = 1e9f, hi = -1e9f;
    for (size_t i = 0; i < r->pixelCount(); ++i) lo = std::min(lo, r->pixel(i)[0]), hi = std::max(hi, r->pixel(i)[0]);
    CHECK(lo >= 0.05f - 1e-6f);
    CHECK(hi <= 4.0f + 1e-5f);
    CHECK(r->pixel(0)[0] == doctest::Approx(0.05f));
    CHECK(r->pixel(32)[0] == doctest::Approx(4.0f));

    // Transparent pixels don't bleed their colour into the edge (premultiplied filtering).
    Image cut(40, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 40; ++x) {
            float* p = cut.pixel(size_t(y) * 40 + x);
            const bool in = x >= 20;
            p[0] = in ? 1.0f : 0.0f, p[1] = in ? 0.0f : 1.0f, p[2] = 0, p[3] = in ? 1.0f : 0.0f;
        }
    auto c = resizeLanczos(cut, 10, 1);
    for (int x = 0; x < 10; ++x)
        if (c->pixel(x)[3] > 0.01f) CHECK(c->pixel(x)[1] == doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("saveRendered: EXR is scene-linear, display formats get the view") {
    const fs::path dir = tempDir("nodelab_saverendered");
    auto img = std::make_shared<Image>(4, 4);
    for (size_t i = 0; i < img->pixelCount(); ++i) {
        float* p = img->pixel(i);
        p[0] = 2.5f, p[1] = 0.5f, p[2] = 0.0f, p[3] = 1;
    }
    SaveOptions o;
    o.format = FileFormat::EXR;
    o.depth = 32;
    std::string err;
    auto firstValues = [&](const fs::path& p) {
        // Uncompressible-small block: find the stored R of pixel 0 by decoding block 0.
        const Bytes b = readAll(p);
        size_t at = 8;
        while (b[at] != 0) {
            while (b[at]) ++at;
            ++at;
            while (b[at]) ++at;
            ++at;
            at += 4 + le32(b, at);
        }
        ++at;
        const size_t off = le32(b, at);
        const uint32_t size = le32(b, off + 4);
        const size_t rawSize = 4 * 4 * 3 * 4;  // 4 lines, B G R, 4 px, float
        Bytes raw;
        if (size < rawSize) {
            Bytes t = inflate(&b[off + 8], size, rawSize);
            for (size_t i = 1; i < t.size(); ++i) t[i] = uint8_t(int(t[i - 1]) + int(t[i]) - 128);
            raw.resize(rawSize);
            for (size_t i = 0; i < rawSize; ++i) raw[i] = t[(i & 1) ? rawSize / 2 + i / 2 : i / 2];
        } else {
            raw.assign(b.begin() + off + 8, b.begin() + off + 8 + rawSize);
        }
        float r, g;
        std::memcpy(&r, &raw[2 * 4 * 4], 4);  // R row of line 0
        std::memcpy(&g, &raw[1 * 4 * 4], 4);
        return std::pair{r, g};
    };
    ColorManagement cm = ColorManagement::sceneLinear();
    cm.view = ColorManagement::AgX;
    REQUIRE(saveRendered(pathToU8(dir / "lin.exr"), img, cm, o, err));
    auto [r, g] = firstValues(dir / "lin.exr");
    CHECK(r == 2.5f);  // no view transform, no clamp
    CHECK(g == 0.5f);
    // A legacy project's values are sRGB-encoded: EXR gets them as linear light.
    REQUIRE(saveRendered(pathToU8(dir / "legacy.exr"), img, ColorManagement{}, o, err));
    auto [r2, g2] = firstValues(dir / "legacy.exr");
    CHECK(g2 == doctest::Approx(colormath::srgbToLinear(0.5f)));
    (void)r2;
    // PNG gets the view transform (AgX maps 2.5 below white).
    SaveOptions png;
    png.depth = 16;
    REQUIRE(saveRendered(pathToU8(dir / "v.png"), img, cm, png, err));
    auto back = loadImage(pathToU8(dir / "v.png"), err);
    REQUIRE(back);
    float want[3];
    const float in[3] = {2.5f, 0.5f, 0.0f};
    colormgmt::viewTransform(cm, in, want);
    CHECK(back->pixel(0)[0] == doctest::Approx(want[0]).epsilon(1e-4));
    CHECK(back->pixel(0)[0] < 1.0f);
    fs::remove_all(dir);
}

TEST_CASE("export formats: settings, extensions and File Output params") {
    ExportSettings s;
    s.format = ExportSettings::EXR;
    s.depth = 32;
    ExportSettings t;
    t.fromJson(s.toJson());
    CHECK(t.format == ExportSettings::EXR);
    CHECK(t.depth == 32);
    CHECK(std::string(t.extension()) == ".exr");
    t.fromJson({{"format", 9}});
    CHECK(t.format == ExportSettings::AVIF);  // out of range: the last format
    CHECK(formatFromPath("a/b.TIFF") == FileFormat::TIFF);
    CHECK(formatFromPath("x.jpeg") == FileFormat::JPEG);
    CHECK(formatFromPath("x.exr") == FileFormat::EXR);
    CHECK(formatFromPath("x.bmp") == FileFormat::PNG);
    CHECK(formatDepth(FileFormat::JPEG, 16) == 8);
    CHECK(formatDepth(FileFormat::EXR, 8) == 16);
    CHECK(formatDepth(FileFormat::TIFF, 32) == 16);

    Graph g;
    Node* n = g.addNode("util.file_output");
    REQUIRE(n);
    // PNG: Color Depth only.
    CHECK(n->paramVisible(3));
    CHECK_FALSE(n->paramVisible(4));
    CHECK_FALSE(n->paramVisible(5));
    n->params[1] = 3;  // OpenEXR
    CHECK_FALSE(n->paramVisible(3));
    CHECK(n->paramVisible(4));
    n->params[1] = 1;  // JPEG
    CHECK(n->paramVisible(5));
}

TEST_CASE("image files are replaced whole, through a temporary file") {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "nodelab_atomic_write";
    fs::create_directories(dir);
    const fs::path out = dir / "out.png";
    Image small(3, 2), big(9, 4);
    for (float& v : small.px) v = 0.25f;
    for (float& v : big.px) v = 0.75f;
    SaveOptions o;
    std::string err;
    REQUIRE(writeImage(pathToU8(out), small, o, err));
    REQUIRE(writeImage(pathToU8(out), big, o, err));
    // The second write replaced the first, and nothing else is left in the folder.
    auto img = loadImage(pathToU8(out), err);
    REQUIRE(img);
    CHECK(img->w == 9);
    CHECK(std::distance(fs::directory_iterator(dir), fs::directory_iterator()) == 1);
    // A folder that doesn't exist fails with an error, and leaves nothing behind.
    CHECK_FALSE(writeImage(pathToU8(dir / "missing" / "x.jpg"), big, o, err));
    CHECK(err == "could not write file");
    fs::remove_all(dir);
}

TEST_CASE("Exports carry the library's title, caption, keywords and rating as XMP") {
    library::Meta m;
    m.title = "Harbour & <fog>";
    m.caption = "Boats at \"dawn\"";
    m.addKeyword("boats");
    m.addKeyword("fog");
    m.rating = 4;
    m.label = library::Green;
    const std::string xmp = library::xmpPacket(m);
    CHECK(xmp.find("<rdf:li xml:lang=\"x-default\">Harbour &amp; &lt;fog&gt;</rdf:li>") != std::string::npos);
    CHECK(xmp.find("Boats at &quot;dawn&quot;") != std::string::npos);
    CHECK(xmp.find("<rdf:li>fog</rdf:li>") != std::string::npos);
    CHECK(xmp.find("xmp:Rating=\"4\"") != std::string::npos);
    CHECK(xmp.find("xmp:Label=\"Green\"") != std::string::npos);
    CHECK(library::xmpPacket(library::Meta{}).empty());
    library::Meta rejected;
    rejected.flag = library::Rejected;
    CHECK(library::xmpPacket(rejected).find("xmp:Rating=\"-1\"") != std::string::npos);

    const fs::path dir = tempDir("nodelab_xmp");
    std::string err;
    for (const FileFormat f : {FileFormat::PNG, FileFormat::JPEG, FileFormat::TIFF, FileFormat::WEBP}) {
        CAPTURE(int(f));
        SaveOptions o;
        o.format = f;
        o.xmp = xmp;
        const fs::path p = dir / (std::string("a") + formatExtension(f));
        REQUIRE(writeImage(pathToU8(p), gradient(16, 12), o, err));
        const Bytes b = readAll(p);
        // Whole and uncompressed, where XMP readers look (as bytes: the packet's BOM is above 0x7F).
        const Bytes packet(xmp.begin(), xmp.end());
        CHECK(std::search(b.begin(), b.end(), packet.begin(), packet.end()) != b.end());
        if (f == FileFormat::PNG) {
            CHECK(contains(b, std::string("iTXtXML:com.adobe.xmp\0\0\0\0\0", 26)));
            CHECK(pngChunksValid(b));
        }
        if (f == FileFormat::JPEG) CHECK(contains(b, std::string("http://ns.adobe.com/xap/1.0/\0", 29)));
        if (f == FileFormat::WEBP) {
            REQUIRE(b.size() > 30);
            CHECK(le32(b, 4) + 8 == b.size());
            CHECK(std::memcmp(&b[12], "VP8X", 4) == 0);
            CHECK((b[20] & 0x04) != 0);  // the XMP flag
            CHECK(contains(b, "XMP "));
        } else {
            auto back = loadImage(pathToU8(p), err);
            REQUIRE(back);
            CHECK(back->w == 16);
        }
    }
    fs::remove_all(dir);
}
