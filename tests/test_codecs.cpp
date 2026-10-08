// WebP (lossy), JPEG XL and AVIF through libwebp, libjxl and libavif: exports read back to their
// pixels (exactly when lossless), carry their profile, EXIF and XMP, and load as images.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/ColorManagement.h"
#include "core/OutputSpace.h"
#include "graph/Graph.h"
#include "io/Export.h"
#include "io/Icc.h"
#include "io/ImageCodecs.h"
#include "io/ImageIO.h"
#include "io/ImageWrite.h"
#include "io/Paths.h"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {

// A smooth photo-like picture with a band of half-transparent pixels.
Image picture(int w, int h, bool alpha) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img.pixel(size_t(y) * w + x);
            p[0] = float(x) / float(w - 1);
            p[1] = 0.5f + 0.4f * std::sin(float(x + y) * 0.15f);
            p[2] = float(y) / float(h - 1);
            p[3] = alpha && y < h / 4 ? 0.5f : 1.0f;
        }
    return img;
}

Bytes fileBytes(const fs::path& p) {
    std::vector<char> f;
    REQUIRE(readFileBytes(pathToU8(p), f));
    return Bytes(f.begin(), f.end());
}

bool contains(const Bytes& hay, const std::string& needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

struct Case {
    FileFormat format;
    int depth;
    const char* name;
};

const Case kLossless[] = {
    {FileFormat::WEBP, 8, "WebP"},
    {FileFormat::JXL, 8, "JPEG XL 8"},
    {FileFormat::JXL, 16, "JPEG XL 16"},
    {FileFormat::AVIF, 8, "AVIF 8"},
    {FileFormat::AVIF, 16, "AVIF 10"},
};

}  // namespace

TEST_CASE("WebP, JPEG XL and AVIF: lossless exports read back exactly") {
    for (const Case& c : kLossless) {
        CAPTURE(std::string(c.name));
        for (bool alpha : {false, true}) {
            CAPTURE(alpha);
            const Image img = picture(37, 23, alpha);
            const fs::path p = fs::temp_directory_path() / (std::string("refractory_codec") + formatExtension(c.format));
            SaveOptions o;
            o.format = c.format;
            o.depth = c.depth;
            o.lossless = true;
            std::string err;
            REQUIRE(writeImage(pathToU8(p), img, o, err));
            const Bytes f = fileBytes(p);
            CHECK(codecs::sniff(f.data(), f.size()) != codecs::Kind::None);
            auto back = loadImage(pathToU8(p), err);
            fs::remove(p);
            REQUIRE(back);
            REQUIRE(back->w == img.w);
            REQUIRE(back->h == img.h);
            // The levels the file stores: 8 bits, 16, or AVIF's 10.
            const float levels = c.depth == 8 ? 255.0f : c.format == FileFormat::AVIF ? 1023.0f : 65535.0f;
            float worst = 0;
            for (size_t i = 0; i < img.px.size(); ++i) {
                const float q = std::round(img.px[i] * levels) / levels;
                worst = std::max(worst, std::abs(back->px[i] - q));
            }
            CHECK(worst < 0.5f / 65535.0f + 1e-6f);
        }
    }
}

TEST_CASE("WebP, JPEG XL and AVIF: lossy exports are close and smaller") {
    // With a photo's fine grain: JPEG XL stores a clean gradient losslessly in a few hundred bytes.
    Image img = picture(160, 96, false);
    uint32_t seed = 1;
    for (size_t i = 0; i < img.px.size(); ++i)
        if (i % 4 != 3) {
            seed = seed * 1664525u + 1013904223u;
            img.px[i] = std::clamp(img.px[i] + (float(seed >> 8) / 16777216.0f - 0.5f) * 0.06f, 0.0f, 1.0f);
        }
    for (FileFormat f : {FileFormat::WEBP, FileFormat::JXL, FileFormat::AVIF}) {
        CAPTURE(std::string(formatExtension(f)));
        const fs::path p = fs::temp_directory_path() / (std::string("refractory_lossy") + formatExtension(f));
        SaveOptions o;
        o.format = f;
        std::string err;
        REQUIRE(writeImage(pathToU8(p), img, o, err));
        const size_t losslessSize = fileBytes(p).size();
        o.lossless = false;
        o.jpegQuality = 80;
        REQUIRE(writeImage(pathToU8(p), img, o, err));
        const size_t lossySize = fileBytes(p).size();
        CHECK(lossySize < losslessSize);
        auto back = loadImage(pathToU8(p), err);
        fs::remove(p);
        REQUIRE(back);
        REQUIRE(back->w == img.w);
        double sum = 0;
        for (size_t i = 0; i < img.px.size(); ++i) sum += std::abs(back->px[i] - img.px[i]);
        CHECK(sum / double(img.px.size()) < 0.02);
        // Alpha survives a lossy picture too (WebP's ALPH chunk).
        const Image a = picture(40, 40, true);
        REQUIRE(writeImage(pathToU8(p), a, o, err));
        back = loadImage(pathToU8(p), err);
        fs::remove(p);
        REQUIRE(back);
        CHECK(back->pixel(0)[3] == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(back->pixel(40 * 39)[3] == doctest::Approx(1.0f));
    }
}

TEST_CASE("WebP, JPEG XL and AVIF carry their colour space, EXIF and XMP") {
    ColorManagement cm = ColorManagement::sceneLinear();
    cm.view = ColorManagement::Standard;
    // A green inside Display P3 but outside sRGB, and a grey, scene-linear.
    icc::Profile p3;
    REQUIRE(icc::parse(iccProfile(outspace::DisplayP3), p3));
    const float g[3] = {0.05f, 0.7f, 0.05f};
    float green[4] = {0, 0, 0, 1};
    for (int r = 0; r < 3; ++r) green[r] = p3.toRec709[r][0] * g[0] + p3.toRec709[r][1] * g[1] + p3.toRec709[r][2] * g[2];
    // Patches big enough that lossy chroma subsampling doesn't mix them: green left, grey right.
    Image scene(32, 16);
    const float grey[4] = {0.18f, 0.18f, 0.18f, 1.0f};
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 32; ++x) std::copy(x < 16 ? green : grey, (x < 16 ? green : grey) + 4, scene.pixel(size_t(y) * 32 + x));
    auto sp = std::make_shared<const Image>(scene);
    DecodeOptions d;
    d.srgbToLinear = d.sceneLinear = d.embeddedProfile = true;

    // A minimal big-endian TIFF structure with no entries stands in for an export's EXIF.
    const Bytes exif = {'M', 'M', 0, 42, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0};
    const std::string xmp = "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">refractory-test</x:xmpmeta>";
    for (FileFormat f : {FileFormat::WEBP, FileFormat::JXL, FileFormat::AVIF}) {
        for (bool lossless : {true, false}) {
            CAPTURE(std::string(formatExtension(f)));
            CAPTURE(lossless);
            const fs::path path = fs::temp_directory_path() / (std::string("refractory_tags") + formatExtension(f));
            SaveOptions o;
            o.format = f;
            o.space = outspace::DisplayP3;
            o.lossless = lossless;
            o.jpegQuality = 95;
            o.exif = exif;
            o.xmp = xmp;
            std::string err;
            REQUIRE(saveRendered(pathToU8(path), sp, cm, o, err));
            const Bytes file = fileBytes(path);
            CHECK(contains(file, "refractory-test"));
            CHECK(contains(file, std::string("MM\0*", 4)));
            CHECK_FALSE(icc::embeddedProfile(pathToU8(path)).empty());
            auto back = loadImage(pathToU8(path), err, d);
            fs::remove(path);
            REQUIRE(back);
            // The green outside sRGB comes back, so the profile was read; grey stays grey.
            const double tol = lossless ? 0.01 : 0.06;
            for (int c = 0; c < 3; ++c) {
                CHECK(back->pixel(8 * 32 + 8)[c] == doctest::Approx(green[c]).epsilon(tol).scale(0.2));
                CHECK(back->pixel(8 * 32 + 24)[c] == doctest::Approx(0.18f).epsilon(tol));
            }
        }
    }
    // An sRGB export has no profile (WebP) or names sRGB.
    const fs::path path = fs::temp_directory_path() / "refractory_tags_srgb.avif";
    SaveOptions o;
    o.format = FileFormat::AVIF;
    std::string err;
    REQUIRE(saveRendered(pathToU8(path), sp, cm, o, err));
    CHECK(icc::embeddedProfile(pathToU8(path)).empty());
    fs::remove(path);
}

TEST_CASE("HDR exports: JPEG XL and AVIF say Rec.2100 PQ, other formats fall back to Rec.2020") {
    ExportSettings s;
    s.colorSpace = outspace::Rec2100PQ;
    for (int f : {int(ExportSettings::JXL), int(ExportSettings::AVIF)}) {
        s.format = f;
        const SaveOptions o = s.saveOptions("", 4, 4);
        CHECK(o.space == outspace::Rec2100PQ);
        CHECK(o.depth == 16);
    }
    s.format = ExportSettings::WEBP;
    CHECK(s.saveOptions("", 4, 4).space == outspace::Rec2020);

    Image img = picture(16, 8, false);
    for (FileFormat f : {FileFormat::JXL, FileFormat::AVIF}) {
        CAPTURE(std::string(formatExtension(f)));
        const fs::path p = fs::temp_directory_path() / (std::string("refractory_pq") + formatExtension(f));
        SaveOptions o;
        o.format = f;
        o.space = outspace::Rec2100PQ;
        o.depth = 16;
        std::string err;
        REQUIRE(writeImage(pathToU8(p), img, o, err));
        // Read back as stored (PQ code values), at full precision.
        auto back = loadImage(pathToU8(p), err);
        REQUIRE(back);
        CHECK(back->pixel(5)[0] == doctest::Approx(img.pixel(5)[0]).epsilon(0.002));
        fs::remove(p);
    }
}

TEST_CASE("Export settings and File Output: JPEG XL, AVIF and lossy modes") {
    CHECK(formatFromPath("a.JXL") == FileFormat::JXL);
    CHECK(formatFromPath("a.avif") == FileFormat::AVIF);
    CHECK(std::string(formatExtension(FileFormat::JXL)) == ".jxl");
    CHECK(std::string(formatExtension(FileFormat::AVIF)) == ".avif");
    CHECK(formatDepth(FileFormat::JXL, 16) == 16);
    CHECK(formatDepth(FileFormat::AVIF, 32) == 16);
    CHECK(isImageFile("x.webp"));
    CHECK(isImageFile("x.jxl"));
    CHECK(isImageFile("x.AVIF"));

    ExportSettings s;
    CHECK(s.lossless);
    s.format = ExportSettings::AVIF;
    s.lossless = false;
    s.jpegQuality = 70;
    ExportSettings t;
    t.fromJson(s.toJson());
    CHECK(t.format == ExportSettings::AVIF);
    CHECK_FALSE(t.lossless);
    CHECK(t.jpegQuality == 70);
    CHECK(t.sameOutput(s));
    // Settings saved before the option are lossless (WebP always was).
    t.fromJson(nlohmann::json{{"format", 4}});
    CHECK(t.format == ExportSettings::WEBP);
    // Lossless files don't care about the quality; lossy ones do.
    ExportSettings a, b;
    a.format = b.format = ExportSettings::JXL;
    b.jpegQuality = 50;
    CHECK(a.sameOutput(b));
    a.lossless = b.lossless = false;
    CHECK_FALSE(a.sameOutput(b));

    Graph g;
    Node* n = g.addNode("util.file_output");
    REQUIRE(n);
    n->params[1] = 5;  // JPEG XL
    CHECK(n->paramVisible(3));
    CHECK(n->paramVisible(6));
    CHECK_FALSE(n->paramVisible(5));  // lossless by default
    n->params[6] = false;
    CHECK(n->paramVisible(5));
    n->params[1] = 0;  // PNG
    CHECK_FALSE(n->paramVisible(6));
}
