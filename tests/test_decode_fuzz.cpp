// Damaged image files through the whole load path (our JPEG, PNG and TIFF decoders, tinyexr, stb_image
// fallback, the embedded-profile scan): random byte flips, cuts, runs of 0xff and spliced bytes.
// Nothing may crash or hang, and an image that loads must have a sane size and finite pixels.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <vector>

#include "io/Icc.h"
#include "io/ImageIO.h"
#include "io/ImageWrite.h"
#include "io/Paths.h"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

Image pattern(int w, int h) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img.pixel(size_t(y) * w + x);
            p[0] = float((x * 5 + y * 3) % 31) / 30.0f;
            p[1] = float(x) / float(w);
            p[2] = float(y) / float(h);
            p[3] = 1.0f;
        }
    return img;
}

Bytes encode(const Image& img, FileFormat format, int depth, bool lossless = true) {
    const fs::path p = fs::temp_directory_path() / (std::string("nodelab_fuzz_src") + formatExtension(format));
    SaveOptions o;
    o.format = format;
    o.depth = depth;
    o.jpegQuality = 80;
    o.lossless = lossless;
    std::string err;
    REQUIRE(writeImage(pathToU8(p), img, o, err));
    std::vector<char> f;
    REQUIRE(readFileBytes(pathToU8(p), f));
    fs::remove(p);
    return Bytes(f.begin(), f.end());
}

Bytes mutate(const Bytes& good, std::mt19937& rng) {
    Bytes b = good;
    auto at = [&](size_t lo) { return lo + rng() % (b.size() - lo); };
    switch (rng() % 5) {
        case 0:  // a few flipped bytes, past the signature
            for (int k = 1 + int(rng() % 8); k > 0; --k) b[at(8)] ^= uint8_t(1 + rng() % 255);
            break;
        case 1:  // cut short
            b.resize(at(1));
            break;
        case 2: {  // a run of 0xff (markers in JPEG, huge lengths in PNG)
            const size_t i = at(2);
            for (size_t j = i; j < i + 1 + rng() % 6 && j < b.size(); ++j) b[j] = 0xff;
            break;
        }
        case 3: {  // a slice copied from elsewhere in the file
            const size_t from = at(0), to = at(2), n = std::min<size_t>(1 + rng() % 64, b.size() - from);
            b.insert(b.begin() + long(to), good.begin() + long(from), good.begin() + long(from + n));
            break;
        }
        default: {  // bytes removed from the middle
            const size_t i = at(2), n = std::min<size_t>(1 + rng() % 64, b.size() - i);
            b.erase(b.begin() + long(i), b.begin() + long(i + n));
        }
    }
    return b;
}

void loadDamaged(const Bytes& good, const char* ext, uint32_t seed, int runs) {
    // NODELAB_FUZZ_RUNS sets a longer run for local hunting.
    if (const char* n = std::getenv("NODELAB_FUZZ_RUNS")) runs = std::atoi(n);
    std::mt19937 rng(seed);
    const fs::path p = fs::temp_directory_path() / (std::string("nodelab_fuzz") + ext);
    {
        // The undamaged file must load, or the runs test nothing.
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(good.data()), std::streamsize(good.size()));
    }
    std::string why;
    REQUIRE(loadImage(pathToU8(p), why));
    int loaded = 0;
    for (int run = 0; run < runs; ++run) {
        const Bytes b = mutate(good, rng);
        {
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
        }
        CAPTURE(run);
        std::string err;
        const auto img = loadImage(pathToU8(p), err);
        (void)icc::embeddedProfile(pathToU8(p));
        if (!img) continue;
        ++loaded;
        REQUIRE(img->w > 0);
        REQUIRE(img->h > 0);
        REQUIRE(img->px.size() == size_t(img->w) * img->h * 4);
        bool finite = true;
        for (float v : img->px) finite &= std::isfinite(v);
        CHECK(finite);
    }
    fs::remove(p);
    MESSAGE(std::string(ext) << ": " << loaded << " of " << runs << " damaged files still loaded");
}

}  // namespace

TEST_CASE("damaged JPEG files load or fail cleanly") {
    // Our writer adds restart markers (the parallel path); 4:4:4 at high quality, 4:2:0 below.
    loadDamaged(encode(pattern(97, 61), FileFormat::JPEG, 8), ".jpg", 1, 400);
}

TEST_CASE("damaged PNG files load or fail cleanly") {
    loadDamaged(encode(pattern(83, 37), FileFormat::PNG, 8), ".png", 2, 300);
    loadDamaged(encode(pattern(41, 29), FileFormat::PNG, 16), ".png", 3, 300);
}

TEST_CASE("damaged TIFF and OpenEXR files load or fail cleanly") {
    loadDamaged(encode(pattern(53, 31), FileFormat::TIFF, 8), ".tif", 6, 300);
    loadDamaged(encode(pattern(29, 43), FileFormat::TIFF, 16), ".tif", 7, 200);
    loadDamaged(encode(pattern(47, 19), FileFormat::EXR, 16), ".exr", 8, 300);
    loadDamaged(encode(pattern(23, 37), FileFormat::EXR, 32), ".exr", 9, 200);
}

TEST_CASE("damaged WebP, JPEG XL and AVIF files load or fail cleanly") {
    loadDamaged(encode(pattern(45, 27), FileFormat::WEBP, 8, false), ".webp", 10, 200);
    loadDamaged(encode(pattern(39, 33), FileFormat::JXL, 8), ".jxl", 11, 200);
    loadDamaged(encode(pattern(31, 25), FileFormat::JXL, 16, false), ".jxl", 12, 200);
    loadDamaged(encode(pattern(35, 21), FileFormat::AVIF, 8, false), ".avif", 13, 150);
    loadDamaged(encode(pattern(27, 29), FileFormat::AVIF, 16), ".avif", 14, 150);
}

TEST_CASE("damaged ICC profiles parse or fail cleanly") {
    const double srgb[3][3] = {{0.4361, 0.2225, 0.0139}, {0.3851, 0.7169, 0.0971}, {0.1431, 0.0606, 0.7141}};
    const Bytes good = icc::buildMatrixTrc("fuzz", srgb, 2.2);
    icc::Profile prof;
    REQUIRE(icc::parse(good, prof));
    std::mt19937 rng(5);
    for (int run = 0; run < 2000; ++run) {
        const Bytes b = mutate(good, rng);
        icc::Profile p;
        (void)icc::parse(b, p);
    }
}

// Opt-in, because it needs a camera file: NODELAB_FUZZ_RAW=<a RAW> runs LibRaw and our highlight
// reconstruction on damaged copies of it (NODELAB_FUZZ_RUNS of them, 30 by default).
TEST_CASE("damaged RAW files load or fail cleanly") {
    const char* src = std::getenv("NODELAB_FUZZ_RAW");
    if (!src) return;
    std::vector<char> f;
    REQUIRE(readFileBytes(src, f));
    const fs::path ext = fs::path(src).extension();
    loadDamaged(Bytes(f.begin(), f.end()), ext.string().c_str(), 6, 30);
}
