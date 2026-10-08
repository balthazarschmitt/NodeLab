// RAW detection and failure handling, EXIF orientation for JPEGs, and Image Input's RAW params.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "graph/Graph.h"
#include "io/Exif.h"
#include "io/ImageIO.h"
#include "io/RawDecode.h"
#include "nodes/io/IONodes.h"

namespace fs = std::filesystem;

namespace {

// 3x2 image whose red channel numbers the pixels 0..5 in reading order.
std::shared_ptr<Image> numbered() {
    auto img = std::make_shared<Image>(3, 2);
    for (size_t i = 0; i < 6; ++i) {
        float* p = img->pixel(i);
        p[0] = float(i), p[1] = p[2] = 0, p[3] = 1;
    }
    return img;
}

std::vector<int> ids(const Image& img) {
    std::vector<int> v;
    for (size_t i = 0; i < img.pixelCount(); ++i) v.push_back(int(img.pixel(i)[0]));
    return v;
}

// A JPEG from stb with an Exif APP1 segment (orientation tag only) spliced in after SOI.
void writeJpegWithOrientation(const fs::path& path, int orientation, bool bigEndian) {
    Image img(3, 2);
    for (size_t i = 0; i < 6; ++i) {
        float* p = img.pixel(i);
        p[0] = p[1] = p[2] = 0.5f, p[3] = 1;
    }
    std::string err;
    REQUIRE(saveImage(path.string(), img, err));
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> jpg((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    std::vector<unsigned char> tiff;
    auto put16 = [&](unsigned v) {
        if (bigEndian) tiff.push_back((unsigned char)(v >> 8)), tiff.push_back((unsigned char)v);
        else tiff.push_back((unsigned char)v), tiff.push_back((unsigned char)(v >> 8));
    };
    auto put32 = [&](unsigned v) {
        if (bigEndian) put16(v >> 16), put16(v & 0xFFFF);
        else put16(v & 0xFFFF), put16(v >> 16);
    };
    tiff.push_back(bigEndian ? 'M' : 'I'), tiff.push_back(bigEndian ? 'M' : 'I');
    put16(42), put32(8);                           // header, IFD0 at 8
    put16(2);                                      // two entries
    put16(0x010F), put16(2), put32(1), put32(0);   // Make (ignored)
    put16(0x0112), put16(3), put32(1), put16(unsigned(orientation)), put16(0);
    put32(0);                                      // no next IFD

    std::vector<unsigned char> seg = {0xFF, 0xE1, 0, 0, 'E', 'x', 'i', 'f', 0, 0};
    seg.insert(seg.end(), tiff.begin(), tiff.end());
    const size_t len = seg.size() - 2;
    seg[2] = (unsigned char)(len >> 8), seg[3] = (unsigned char)len;
    jpg.insert(jpg.begin() + 2, seg.begin(), seg.end());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(jpg.data()), std::streamsize(jpg.size()));
}

}  // namespace

TEST_CASE("EXIF orientations turn the stored pixels upright") {
    auto img = numbered();  // 0 1 2 / 3 4 5
    CHECK(exif::applyOrientation(img, 1) == img);
    CHECK(ids(*exif::applyOrientation(img, 2)) == std::vector<int>{2, 1, 0, 5, 4, 3});
    CHECK(ids(*exif::applyOrientation(img, 3)) == std::vector<int>{5, 4, 3, 2, 1, 0});
    CHECK(ids(*exif::applyOrientation(img, 4)) == std::vector<int>{3, 4, 5, 0, 1, 2});
    // 6: the camera was turned clockwise, so rotate 90 CW to view: 2 wide, 3 tall.
    auto r6 = exif::applyOrientation(img, 6);
    CHECK(r6->w == 2);
    CHECK(r6->h == 3);
    CHECK(ids(*r6) == std::vector<int>{3, 0, 4, 1, 5, 2});
    CHECK(ids(*exif::applyOrientation(img, 8)) == std::vector<int>{2, 5, 1, 4, 0, 3});
    CHECK(ids(*exif::applyOrientation(img, 5)) == std::vector<int>{0, 3, 1, 4, 2, 5});
    CHECK(ids(*exif::applyOrientation(img, 7)) == std::vector<int>{5, 2, 4, 1, 3, 0});
}

TEST_CASE("JPEG EXIF orientation is read, and applied only in scene-linear projects") {
    const fs::path path = fs::temp_directory_path() / "refractory_exif.jpg";
    for (bool be : {false, true}) {
        CAPTURE(be);
        writeJpegWithOrientation(path, 6, be);
        CHECK(exif::jpegOrientation(path.string()) == 6);
    }
    std::string err;
    auto legacy = loadImage(path.string(), err, DecodeOptions{false, false});
    REQUIRE(legacy);
    CHECK(legacy->w == 3);  // legacy projects keep files as stored
    int fw = 0, fh = 0;
    auto upright = loadImage(path.string(), err, DecodeOptions{true, true}, false, &fw, &fh);
    REQUIRE(upright);
    CHECK(upright->w == 2);
    CHECK(upright->h == 3);
    CHECK(fw == 2);
    CHECK(fh == 3);

    Image plain(2, 2);
    REQUIRE(saveImage(path.string(), plain, err));
    CHECK(exif::jpegOrientation(path.string()) == 1);
    CHECK(exif::jpegOrientation((fs::temp_directory_path() / "refractory_missing.jpg").string()) == 1);
    fs::remove(path);
}

TEST_CASE("RAW files are recognised by extension and fail cleanly when broken") {
    CHECK(raw::isRawPath("C:/x/IMG_0001.CR2"));
    CHECK(raw::isRawPath("a.nef"));
    CHECK(raw::isRawPath("a.dng"));
    CHECK_FALSE(raw::isRawPath("a.jpg"));
    CHECK(isImageFile("a.ARW"));
    CHECK(isImageFile("a.png"));
    CHECK_FALSE(isImageFile("a.txt"));

    const fs::path path = fs::temp_directory_path() / "refractory_broken.cr2";
    {
        std::ofstream f(path, std::ios::binary);
        f << "definitely not a raw file";
    }
    std::string err;
    CHECK(loadImage(path.string(), err, DecodeOptions{}) == nullptr);
    CHECK_FALSE(err.empty());
    fs::remove(path);
    err.clear();
    CHECK(raw::load((fs::temp_directory_path() / "refractory_missing.cr2").string(), err, raw::Blend, false) == nullptr);
    CHECK_FALSE(err.empty());
}

TEST_CASE("Image Input shows Highlight Reconstruction for RAW files and Color Space for others") {
    Graph g;
    Node* n = g.addNode("io.image_input");
    REQUIRE(n);
    n->params[0] = "photo.jpg";
    CHECK(n->paramVisible(1));
    CHECK_FALSE(n->paramVisible(2));
    n->params[0] = "photo.CR2";
    CHECK_FALSE(n->paramVisible(1));
    CHECK(n->paramVisible(2));
    CHECK(static_cast<const ImageInputNode&>(*n).decode(true).rawHighlights == raw::Reconstruct);
}

TEST_CASE("Choosing a RAW sets darktable-style Baseline Exposure defaults that old projects don't get") {
    Graph g;
    auto& in = static_cast<ImageInputNode&>(*g.addNode("io.image_input"));
    // Defaults (and so projects saved before the params existed) change nothing.
    CHECK(in.paramF(3) == 0.0f);
    CHECK_FALSE(in.paramB(4));
    in.params[0] = "old.CR2";
    CHECK(in.exposureGain() == 1.0f);

    in.params[0] = "";
    CHECK(in.chooseFile("photo.CR2"));
    CHECK(in.paramF(3) == doctest::Approx(ImageInputNode::kRawBaselineEV));
    CHECK(in.paramB(4));
    CHECK(in.paramVisible(3));
    CHECK(in.paramVisible(4));
    // A missing file has no exposure bias, so the gain is just the baseline.
    CHECK(in.exposureGain() == doctest::Approx(std::exp2(ImageInputNode::kRawBaselineEV)));

    // RAW to RAW keeps the user's settings; JPEGs ignore them.
    in.params[3] = -0.5f;
    CHECK_FALSE(in.chooseFile("other.NEF"));
    CHECK(in.paramF(3) == doctest::Approx(-0.5f));
    CHECK_FALSE(in.chooseFile("photo.jpg"));
    CHECK_FALSE(in.paramVisible(3));
    CHECK(in.exposureGain() == 1.0f);

    // The gain scales colour, not alpha.
    in.params[0] = "photo.dng";
    in.params[3] = 1.0f;
    in.params[4] = false;
    auto img = std::make_shared<Image>(1, 1);
    float* p = img->pixel(0);
    p[0] = 0.25f, p[1] = 0.5f, p[2] = 1.5f, p[3] = 0.75f;
    ImagePtr out = in.applyExposure(img);
    REQUIRE(out != img);
    CHECK(out->pixel(0)[0] == doctest::Approx(0.5f));
    CHECK(out->pixel(0)[2] == doctest::Approx(3.0f));
    CHECK(out->pixel(0)[3] == doctest::Approx(0.75f));
    CHECK(img->pixel(0)[0] == 0.25f);  // the cached source is untouched
}

// Opt-in, because it needs a camera file: REFRACTORY_FUZZ_RAW=<a RAW>. A half-size decode of a sensor
// with an odd width or height (CR3s are 5999x3999) left its last row and column a colour short,
// a green or yellow line on the preview's edge, and the EOS 70D's junk last row made a cyan line at
// any size. Each edge must look like the lines next to it.
TEST_CASE("RAW decodes have no off-colour edge lines") {
    const char* src = std::getenv("REFRACTORY_FUZZ_RAW");
    if (!src) return;
    for (const bool half : {true, false}) {
    CAPTURE(half);
    std::string err;
    auto img = raw::load(src, err, raw::Blend, half);
    REQUIRE(img);
    auto mean = [&](bool column, int i) {
        std::array<double, 3> m{};
        const int n = column ? img->h : img->w;
        for (int k = 0; k < n; ++k) {
            const float* p = img->pixel(column ? size_t(k) * img->w + i : size_t(i) * img->w + k);
            for (int c = 0; c < 3; ++c) m[size_t(c)] += p[c] / n;
        }
        return m;
    };
    auto near = [&](bool column, int edge, int inner) {
        const auto a = mean(column, edge), b = mean(column, inner);
        for (int c = 0; c < 3; ++c) CHECK(std::abs(a[size_t(c)] - b[size_t(c)]) < 0.2 * std::max(b[size_t(c)], 0.01) + 0.005);
    };
    // Two lines deep, against lines past the reach of demosaicing's smear.
    for (int k = 0; k < 2; ++k) {
        near(true, k, 4);
        near(true, img->w - 1 - k, img->w - 5);
        near(false, k, 4);
        near(false, img->h - 1 - k, img->h - 5);
    }
    }
}

TEST_CASE("RAW junk edge lines are replaced by the nearest line of their colours, real edges kept") {
    const int W = 40, H = 30, pitch = 48, left = 4, top = 2, black = 2048, white = 15000;
    std::vector<uint16_t> raw(size_t(pitch) * 36);
    // A Bayer-like mosaic: two colours alternating along each line, with a gentle gradient down
    // and across, as a picture has.
    auto at = [&](int x, int y) -> uint16_t& { return raw[size_t(top + y) * pitch + left + x]; };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) at(x, y) = uint16_t(3000 + 40 * y + 15 * x + ((x + y) & 1 ? 600 : 0));
    auto clean = raw;
    CHECK(raw::repairEdgeLines(raw.data(), pitch, left, top, W, H, black, white) == 0);
    CHECK(raw == clean);
    // The 70D's last row: near white.
    for (int x = 0; x < W; ++x) at(x, H - 1) = 13800;
    // A dead first column.
    for (int y = 0; y < H; ++y) at(0, y) = 0;
    CHECK(raw::repairEdgeLines(raw.data(), pitch, left, top, W, H, black, white) == 2);
    for (int x = 0; x < W; ++x) CHECK(at(x, H - 1) == at(x, H - 3));
    for (int y = 1; y < H - 1; ++y) CHECK(at(0, y) == at(2, y));
    // Nothing outside the visible area or inside it changed.
    for (int y = 0; y < 36; ++y)
        for (int x = 0; x < pitch; ++x) {
            const int vx = x - left, vy = y - top;
            if (vy == H - 1 || vx == 0) continue;
            CHECK(raw[size_t(y) * pitch + x] == clean[size_t(y) * pitch + x]);
        }
    // In a night shot the same row reads only about twice the signal above black.
    raw = clean;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) at(x, y) = uint16_t(black + 130 + ((x + y) & 1 ? 40 : 0));
    for (int x = 0; x < W; ++x) at(x, H - 1) = uint16_t(at(x, H - 1) + 130);
    auto dark = raw;
    CHECK(raw::repairEdgeLines(raw.data(), pitch, left, top, W, H, black, white) == 1);
    for (int x = 0; x < W; ++x) CHECK(at(x, H - 1) == at(x, H - 3));
    // A real dark edge (a vignette's gradient) stays.
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) at(x, y) = uint16_t(black + 100 + 20 * std::min(x, 6) + 20 * std::min(y, 6));
    dark = raw;
    CHECK(raw::repairEdgeLines(raw.data(), pitch, left, top, W, H, black, white) == 0);
    CHECK(raw == dark);
}
