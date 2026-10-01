// Embedded ICC profiles on input: parsing, the sRGB fast path, and P3 / Adobe RGB files decoding to
// the right linear Rec.709 colours.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include <zlib.h>

#include "graph/Graph.h"
#include "io/Icc.h"
#include "io/ImageIO.h"
#include "io/ImageWrite.h"
#include "nodes/io/IONodes.h"

namespace fs = std::filesystem;

namespace {

// Colorants in the D50 connection space, as in Apple's Display P3 and Adobe's Adobe RGB (1998).
const double kP3[3][3] = {{0.515121, 0.241196, -0.001053}, {0.291977, 0.692245, 0.041885}, {0.157104, 0.066574, 0.784073}};
const double kAdobe[3][3] = {{0.60974, 0.31111, 0.01947}, {0.20528, 0.62567, 0.06087}, {0.14919, 0.06322, 0.74457}};
const double kSrgb[3][3] = {{0.4360747, 0.2225045, 0.0139322}, {0.3850649, 0.7168786, 0.0971045}, {0.1430804, 0.0606169, 0.7141733}};

using Bytes = std::vector<uint8_t>;

Bytes readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void writeAll(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}
void put32(Bytes& o, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s));
}

// A 2x1 image: pure red, then mid grey (8-bit codes 255,0,0 and 128,128,128).
Image testImage() {
    Image img(2, 1);
    float* a = img.pixel(0);
    a[0] = 1, a[1] = 0, a[2] = 0, a[3] = 1;
    float* b = img.pixel(1);
    b[0] = b[1] = b[2] = 128.0f / 255.0f, b[3] = 1;
    return img;
}

// Writes a PNG with an iCCP chunk holding `profile` (inserted after IHDR).
fs::path pngWithProfile(const char* name, const Bytes& profile) {
    const fs::path p = fs::temp_directory_path() / name;
    std::string err;
    REQUIRE(saveImage(p.string(), testImage(), err));
    Bytes f = readAll(p);
    Bytes z(compressBound(uLong(profile.size())));
    uLongf zn = uLongf(z.size());
    REQUIRE(compress(z.data(), &zn, profile.data(), uLong(profile.size())) == Z_OK);
    Bytes data = {'i', 'c', 'c', 0, 0};
    data.insert(data.end(), z.begin(), z.begin() + zn);
    Bytes chunk;
    put32(chunk, uint32_t(data.size()));
    Bytes typed = {'i', 'C', 'C', 'P'};
    typed.insert(typed.end(), data.begin(), data.end());
    chunk.insert(chunk.end(), typed.begin(), typed.end());
    put32(chunk, uint32_t(crc32(0, typed.data(), uInt(typed.size()))));
    const size_t afterIhdr = 8 + 4 + 4 + 13 + 4;
    f.insert(f.begin() + afterIhdr, chunk.begin(), chunk.end());
    writeAll(p, f);
    return p;
}

// Writes a JPEG with the profile in two APP2 segments (as long profiles are split).
fs::path jpegWithProfile(const char* name, const Bytes& profile) {
    const fs::path p = fs::temp_directory_path() / name;
    std::string err;
    REQUIRE(saveImage(p.string(), testImage(), err, 100));
    Bytes f = readAll(p);
    const size_t half = profile.size() / 2;
    Bytes segs;
    for (int k = 0; k < 2; ++k) {
        const auto b = profile.begin() + (k ? half : 0), e = k ? profile.end() : profile.begin() + half;
        const size_t len = 2 + 14 + size_t(e - b);
        segs.insert(segs.end(), {0xFF, 0xE2, uint8_t(len >> 8), uint8_t(len)});
        const char* sig = "ICC_PROFILE";
        segs.insert(segs.end(), sig, sig + 12);  // with its NUL
        segs.push_back(uint8_t(k + 1));
        segs.push_back(2);
        segs.insert(segs.end(), b, e);
    }
    f.insert(f.begin() + 2, segs.begin(), segs.end());
    writeAll(p, f);
    return p;
}

DecodeOptions withProfile(bool on) {
    DecodeOptions d;
    d.srgbToLinear = true;
    d.sceneLinear = true;
    d.embeddedProfile = on;
    return d;
}

}  // namespace

TEST_CASE("ICC matrix/TRC profiles parse to a Rec.709 conversion") {
    icc::Profile p;
    REQUIRE(icc::parse(icc::buildMatrixTrc("Display P3", kP3, -1), p));
    CHECK(p.name == "Display P3");
    CHECK_FALSE(icc::isSrgb(p));
    // P3's red is outside Rec.709: more red, slightly negative green and blue.
    CHECK(p.toRec709[0][0] == doctest::Approx(1.2249).epsilon(0.002));
    CHECK(p.toRec709[1][0] == doctest::Approx(-0.0420).epsilon(0.05));
    CHECK(p.toRec709[2][0] == doctest::Approx(-0.0196).epsilon(0.08));
    for (int i = 0; i < 3; ++i)  // white stays white
        CHECK(p.toRec709[i][0] + p.toRec709[i][1] + p.toRec709[i][2] == doctest::Approx(1.0f));

    icc::Profile adobe;
    REQUIRE(icc::parse(icc::buildMatrixTrc("Adobe RGB (1998)", kAdobe, 563.0 / 256.0), adobe));
    CHECK(adobe.trc[0].eval(0.5f) == doctest::Approx(std::pow(0.5f, 563.0f / 256.0f)).epsilon(1e-4));
    CHECK_FALSE(icc::isSrgb(adobe));

    // sRGB profiles (ours, and one built from the same colorants) take the plain sRGB path.
    icc::Profile s;
    REQUIRE(icc::parse(srgbIccProfile(), s));
    CHECK(icc::isSrgb(s));
    REQUIRE(icc::parse(icc::buildMatrixTrc("sRGB", kSrgb, -1), s));
    CHECK(icc::isSrgb(s));

    std::string why;
    CHECK_FALSE(icc::parse(Bytes(200, 0), p, &why));
    CHECK_FALSE(why.empty());
}

TEST_CASE("Images with an embedded P3 or Adobe RGB profile decode through it") {
    const Bytes p3 = icc::buildMatrixTrc("Display P3", kP3, -1);
    for (const fs::path& path : {pngWithProfile("nodelab_icc_p3.png", p3), jpegWithProfile("nodelab_icc_p3.jpg", p3)}) {
        CAPTURE(path.string());
        CHECK(icc::embeddedProfile(path.string()) == p3);
        CHECK(embeddedProfileInfo(path.string()) == "Display P3");
        std::string err;
        auto img = loadImage(path.string(), err, withProfile(true));
        auto plain = loadImage(path.string(), err, withProfile(false));
        REQUIRE(img);
        REQUIRE(plain);
        const bool jpeg = path.extension() == ".jpg";
        const float tol = jpeg ? 0.03f : 0.002f;  // JPEG's chroma subsampling smears the 2 pixels
        if (!jpeg) {
            // P3 red is more saturated than Rec.709's.
            CHECK(img->pixel(0)[0] == doctest::Approx(1.2249f).epsilon(0.003));
            CHECK(img->pixel(0)[1] < -0.03f);
            CHECK(plain->pixel(0)[0] == doctest::Approx(1.0f));
        }
        // Grey stays grey, at the sRGB curve's level (the P3 profile uses that curve).
        const float* g = img->pixel(1);
        CHECK(g[0] == doctest::Approx(plain->pixel(1)[0]).epsilon(tol));
        CHECK(std::fabs(g[0] - g[2]) < tol);
        CHECK(g[3] == 1.0f);
    }

    // Adobe RGB has a 2.2 curve: mid grey decodes darker than through sRGB's.
    const fs::path adobe = pngWithProfile("nodelab_icc_adobe.png", icc::buildMatrixTrc("Adobe RGB (1998)", kAdobe, 563.0 / 256.0));
    std::string err;
    auto img = loadImage(adobe.string(), err, withProfile(true));
    REQUIRE(img);
    CHECK(img->pixel(1)[1] == doctest::Approx(std::pow(128.0f / 255.0f, 563.0f / 256.0f)).epsilon(0.002));

    // An sRGB-tagged file decodes exactly like an untagged one.
    const fs::path srgb = pngWithProfile("nodelab_icc_srgb.png", srgbIccProfile());
    auto a = loadImage(srgb.string(), err, withProfile(true));
    auto b = loadImage(srgb.string(), err, withProfile(false));
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->px == b->px);
    CHECK(embeddedProfileInfo(srgb.string()).empty());
}

TEST_CASE("Image Input applies embedded profiles only to sRGB images in scene-linear projects") {
    Graph g;
    auto& in = static_cast<ImageInputNode&>(*g.addNode("io.image_input"));
    in.params[0] = "photo.jpg";
    CHECK(in.paramB(5));
    CHECK(in.paramVisible(5));
    CHECK(in.decode(true).embeddedProfile);
    CHECK_FALSE(in.decode(false).embeddedProfile);  // legacy projects read files as stored
    in.params[1] = 2;                               // Non-Color
    CHECK_FALSE(in.decode(true).embeddedProfile);
    CHECK_FALSE(in.paramVisible(5));
    in.params[1] = 0;
    in.params[5] = false;
    CHECK_FALSE(in.decode(true).embeddedProfile);
    in.params[0] = "photo.CR2";
    CHECK_FALSE(in.paramVisible(5));
}
