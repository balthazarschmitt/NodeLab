// RAW detection and failure handling, EXIF orientation for JPEGs, and Image Input's RAW params.
#include <doctest/doctest.h>

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
    const fs::path path = fs::temp_directory_path() / "nodelab_exif.jpg";
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
    CHECK(exif::jpegOrientation((fs::temp_directory_path() / "nodelab_missing.jpg").string()) == 1);
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

    const fs::path path = fs::temp_directory_path() / "nodelab_broken.cr2";
    {
        std::ofstream f(path, std::ios::binary);
        f << "definitely not a raw file";
    }
    std::string err;
    CHECK(loadImage(path.string(), err, DecodeOptions{}) == nullptr);
    CHECK_FALSE(err.empty());
    fs::remove(path);
    err.clear();
    CHECK(raw::load((fs::temp_directory_path() / "nodelab_missing.cr2").string(), err, raw::Blend, false) == nullptr);
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
