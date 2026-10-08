// The Library's metadata: labels, titles, captions, keywords, stacks, search, collections and
// duplicate detection.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

#include "io/ImageIO.h"
#include "io/Library.h"
#include "io/Paths.h"

namespace fs = std::filesystem;

namespace {

// A picture made of smooth blobs, different per seed.
Image scene(int w, int h, int seed) {
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float u = float(x) / w, v = float(y) / h;
            const float l = 0.5f + 0.25f * std::sin(6.3f * u * (1 + seed) + seed) * std::cos(4.1f * v * (2 + seed) - seed) +
                            0.2f * std::sin(9.0f * (u + v) + 1.7f * seed);
            float* p = img.pixel(size_t(y) * size_t(w) + size_t(x));
            p[0] = l, p[1] = l * 0.9f, p[2] = l * 0.8f, p[3] = 1;
        }
    return img;
}

// Box-averaged to half size, as a re-saved smaller copy would be.
Image half(const Image& src) {
    Image out(src.w / 2, src.h / 2);
    for (int y = 0; y < out.h; ++y)
        for (int x = 0; x < out.w; ++x)
            for (int c = 0; c < 4; ++c) {
                float s = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) s += src.pixel(size_t(2 * y + dy) * size_t(src.w) + size_t(2 * x + dx))[c];
                out.pixel(size_t(y) * size_t(out.w) + size_t(x))[c] = s / 4;
            }
    return out;
}

}  // namespace

TEST_CASE("Library meta: labels, title, caption, keywords and stack round-trip; old sidecars still read") {
    library::Meta m;
    m.rating = 4;
    m.label = library::Green;
    m.title = "Harbour";
    m.caption = "Morning fog over the boats";
    m.addKeyword(" boats ");
    m.addKeyword("Fog");
    m.addKeyword("fog");  // a repeat, ignoring case
    m.addKeyword("");
    m.stack = "IMG_1.jpg";
    CHECK(m.keywords == std::vector<std::string>{"boats", "Fog"});
    CHECK(m.hasKeyword("FOG"));
    const library::Meta back = library::Meta::fromJson(m.toJson());
    CHECK(back.rating == 4);
    CHECK(back.label == library::Green);
    CHECK(back.title == m.title);
    CHECK(back.caption == m.caption);
    CHECK(back.keywords == m.keywords);
    CHECK(back.stack == m.stack);
    m.removeKeyword("BOATS");
    CHECK(m.keywords == std::vector<std::string>{"Fog"});

    // Sidecars written before these fields: everything new at its default.
    const library::Meta old = library::Meta::fromJson(nlohmann::json{{"rating", 2}, {"flag", 1}});
    CHECK(old.rating == 2);
    CHECK(old.flag == library::Picked);
    CHECK(old.label == library::NoLabel);
    CHECK(old.keywords.empty());
    CHECK(old.stack.empty());
    // Damaged fields are skipped one by one.
    const library::Meta bad = library::Meta::fromJson(nlohmann::json{{"rating", 3}, {"label", "red"}, {"keywords", 5}, {"title", 1}});
    CHECK(bad.rating == 3);
    CHECK(bad.label == library::NoLabel);
    CHECK(bad.keywords.empty());
    // Only what's set is written, so unlabelled sidecars stay as they were.
    const nlohmann::json plain = library::Meta{}.toJson();
    CHECK_FALSE(plain.contains("label"));
    CHECK_FALSE(plain.contains("keywords"));
    CHECK_FALSE(plain.contains("stack"));
}

TEST_CASE("Library keywords split, and search matches every word") {
    CHECK(library::splitKeywords("a, b ;c,, ") == std::vector<std::string>{"a", "b", "c"});
    library::Meta m;
    m.title = "Old Harbour";
    m.addKeyword("Boats");
    const std::string photo = "C:/Photos/IMG_0042.jpg";
    CHECK(library::matchesSearch(photo, m, ""));
    CHECK(library::matchesSearch(photo, m, "harbour"));
    CHECK(library::matchesSearch(photo, m, "boats img_0042"));
    CHECK(library::matchesSearch(photo, m, "  BOAT  "));
    CHECK_FALSE(library::matchesSearch(photo, m, "boats sunset"));
    CHECK_FALSE(library::matchesSearch(photo, m, "photos"));  // the folder isn't searched
}

TEST_CASE("Library collections save, load and add without repeats") {
    const fs::path file = fs::temp_directory_path() / "refractory_tests_collections.json";
    std::error_code ec;
    fs::remove(file, ec);
    CHECK(library::loadCollections().empty());
    std::vector<library::Collection> c;
    CHECK(library::addToCollection(c, "Best", {{"C:/a.jpg", 0}, {"C:/b.jpg", 2}}) == 2);
    CHECK(library::addToCollection(c, "Best", {{"C:/a.jpg", 0}, {"C:/a.jpg", 1}}) == 1);
    CHECK(library::addToCollection(c, "Print", {{"D:/c.png", 0}}) == 1);
    std::string err;
    REQUIRE(library::saveCollections(c, err));
    const auto back = library::loadCollections();
    REQUIRE(back.size() == 2);
    CHECK(back[0].name == "Best");
    REQUIRE(back[0].entries.size() == 3);
    CHECK(back[0].entries[1].photo == "C:/b.jpg");
    CHECK(back[0].entries[1].copy == 2);
    CHECK(back[1].entries[0].photo == "D:/c.png");
    // A damaged file reads as no collections, not a crash.
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << "{\"collections\": [ {\"name\": 3}, {\"name\": \"x\", \"photos\": [1, {\"copy\": 2}, \"C:/d.jpg\"]} ";
    }
    CHECK(library::loadCollections().empty());
    fs::remove(file, ec);
}

TEST_CASE("Library duplicates: the same file, and the same picture resized, but not a different one") {
    const Image a = scene(240, 160, 1), aSmall = half(a), b = scene(240, 160, 2);
    CHECK(library::hashDistance(library::pictureHash(a), library::pictureHash(aSmall)) <= 2);
    CHECK(library::hashDistance(library::pictureHash(a), library::pictureHash(b)) > 10);

    // Through files and thumbnails, as Find Duplicates runs.
    const fs::path dir = fs::temp_directory_path() / "refractory_dups";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    std::string err;
    const std::vector<std::string> paths = {pathToU8(dir / "a.png"), pathToU8(dir / "b.png"), pathToU8(dir / "a copy.png"),
                                            pathToU8(dir / "a small.png")};
    REQUIRE(saveImage(paths[0], a, err));
    REQUIRE(saveImage(paths[1], b, err));
    fs::copy_file(dir / "a.png", dir / "a copy.png");
    REQUIRE(saveImage(paths[3], aSmall, err));
    std::vector<uint64_t> files, pictures;
    std::vector<float> aspects;
    for (const std::string& p : paths) {
        files.push_back(library::fileHash(p));
        const ImagePtr t = library::loadThumbnail(p, 64, err);
        REQUIRE(t);
        pictures.push_back(library::pictureHash(*t));
        aspects.push_back(float(t->w) / float(t->h));
    }
    CHECK(files[0] == files[2]);
    CHECK(files[0] != files[3]);
    const auto groups = library::groupDuplicates(files, pictures, aspects);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0] == std::vector<int>{0, 2, 3});

    // A different shape never matches by picture.
    CHECK(library::groupDuplicates({1, 2}, {pictures[0], pictures[0]}, {1.5f, 1.0f}).empty());
    CHECK(library::fileHash(pathToU8(dir / "missing.png")) == 0);
    fs::remove_all(dir, ec);
}

TEST_CASE("Library duplicates: rotated copies match with the rotation hashes, and only with them") {
    const Image a = scene(240, 160, 3), b = scene(240, 160, 4);
    // a turned clockwise by a quarter (portrait) and by a half.
    Image q(a.h, a.w), h(a.w, a.h);
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x)
            for (int c = 0; c < 4; ++c) {
                const float v = a.pixel(size_t(y) * size_t(a.w) + size_t(x))[c];
                q.pixel(size_t(x) * size_t(q.w) + size_t(a.h - 1 - y))[c] = v;
                h.pixel(size_t(a.h - 1 - y) * size_t(h.w) + size_t(a.w - 1 - x))[c] = v;
            }
    const std::vector<Image> imgs = {a, q, h, b};
    std::vector<uint64_t> files = {1, 2, 3, 4}, pictures;
    std::vector<float> aspects;
    std::vector<std::array<uint64_t, 3>> rotated;
    for (const Image& img : imgs) {
        pictures.push_back(library::pictureHash(img));
        aspects.push_back(float(img.w) / float(img.h));
        rotated.push_back(library::rotatedHashes(img));
    }
    // The upright hash of a turned copy is its original's.
    CHECK(library::hashDistance(library::rotatedHashes(a)[0], pictures[1]) == 0);
    CHECK(library::hashDistance(library::rotatedHashes(a)[1], pictures[2]) == 0);
    CHECK(library::groupDuplicates(files, pictures, aspects).empty());
    const auto groups = library::groupDuplicates(files, pictures, aspects, 4, rotated);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0] == std::vector<int>{0, 1, 2});
    // A flat picture's turns don't match everything flat.
    Image flat(40, 40);
    CHECK(library::rotatedHashes(flat) == std::array<uint64_t, 3>{});
}
