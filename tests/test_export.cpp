#include <doctest/doctest.h>

#include <filesystem>

#include "graph/Graph.h"
#include "io/Export.h"
#include "io/ImageIO.h"
#include "io/Paths.h"

namespace fs = std::filesystem;

namespace {

// A flat-coloured opaque PNG in a fresh temp folder.
std::string writeSource(const fs::path& dir, const std::string& name, float v, int w = 40, int h = 20) {
    Image img(w, h);
    for (size_t i = 0; i < img.pixelCount(); ++i) {
        float* p = img.pixel(i);
        p[0] = v, p[1] = v * 0.5f, p[2] = 0.25f, p[3] = 1.0f;
    }
    std::string err;
    const std::string path = pathToU8(dir / name);
    REQUIRE(saveImage(path, img, err));
    return path;
}

}  // namespace

TEST_CASE("export settings round-trip and clamp") {
    ExportSettings s;
    s.format = ExportSettings::JPEG;
    s.jpegQuality = 80;
    s.sizeMode = ExportSettings::LongEdge;
    s.longEdge = 1000;
    s.suffix = "_x";
    ExportSettings t;
    t.fromJson(s.toJson());
    CHECK(t.format == ExportSettings::JPEG);
    CHECK(t.jpegQuality == 80);
    CHECK(t.longEdge == 1000);
    CHECK(t.suffix == "_x");
    t.fromJson({{"jpegQuality", 500}, {"percent", -3}});
    CHECK(t.jpegQuality == 100);
    CHECK(t.percent == 1);
}

TEST_CASE("resizeForExport only shrinks") {
    auto img = std::make_shared<const Image>(400, 200);
    ExportSettings s;
    CHECK(resizeForExport(img, s) == img);
    s.sizeMode = ExportSettings::LongEdge;
    s.longEdge = 100;
    auto r = resizeForExport(img, s);
    CHECK(r->w == 100);
    CHECK(r->h == 50);
    s.longEdge = 4000;
    CHECK(resizeForExport(img, s)->w == 400);
    s.sizeMode = ExportSettings::Percent;
    s.percent = 25;
    CHECK(resizeForExport(img, s)->w == 100);
}

TEST_CASE("Output Sharpening is off by default, and stronger for paper and higher amounts") {
    auto img = std::make_shared<Image>(64, 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 64; ++x) {
            float* p = img->pixel(size_t(y) * 64 + x);
            p[0] = p[1] = p[2] = x < 32 ? 0.2f : 0.6f;
            p[3] = 1.0f;
        }
    ImagePtr src = img;
    ExportSettings s;
    CHECK(sharpenForExport(src, s, true) == src);
    // Overshoot on the dark side of the edge.
    auto dip = [&](int target, int amount) {
        s.sharpenFor = target, s.sharpenAmount = amount;
        return 0.2f - sharpenForExport(src, s, true)->pixel(4 * 64 + 31)[0];
    };
    CHECK(dip(ExportSettings::Screen, 0) > 0.0f);
    CHECK(dip(ExportSettings::Screen, 2) > dip(ExportSettings::Screen, 0));
    CHECK(dip(ExportSettings::Matte, 1) > dip(ExportSettings::Screen, 1));
    ExportSettings t;
    t.fromJson(s.toJson());
    CHECK(t.sharpenFor == ExportSettings::Matte);
    CHECK(t.sharpenAmount == 1);
}

TEST_CASE("batchOutputPath never overwrites the source") {
    ExportSettings s;
    s.suffix = "";
    const fs::path dir = fs::temp_directory_path() / "nodelab_batch_name";
    fs::create_directories(dir);
    const std::string src = writeSource(dir, "a.png", 0.5f);
    const std::string same = batchOutputPath(src, pathToU8(dir), s);
    CHECK(u8ToPath(same).filename() == "a_edit.png");
    s.format = ExportSettings::JPEG;
    CHECK(u8ToPath(batchOutputPath(src, pathToU8(dir), s)).filename() == "a.jpg");
    fs::remove_all(dir);
}

TEST_CASE("Exporter runs a batch through the graph on a background thread") {
    const fs::path dir = fs::temp_directory_path() / "nodelab_batch_run";
    fs::remove_all(dir);
    fs::create_directories(dir / "out");
    const std::string a = writeSource(dir, "a.png", 1.0f, 40, 20);
    const std::string b = writeSource(dir, "b.png", 0.0f, 30, 30);

    Graph g;
    Node* in = g.addNode("io.image_input");
    Node* inv = g.addNode("color.invert");
    Node* out = g.addNode("io.output");
    g.connect(in->id, 0, inv->id, 0);
    g.connect(inv->id, 0, out->id, 0);

    ExportSettings s;  // PNG, "_edit"
    std::vector<ExportItem> items;
    for (const std::string& src : {a, b, pathToU8(dir / "missing.png")})
        items.push_back({src, batchOutputPath(src, pathToU8(dir / "out"), s)});
    Exporter ex;
    ex.start(g.toJson(), items, in->id, s);
    ex.wait();
    CHECK_FALSE(ex.busy());
    const Exporter::Progress pr = ex.progress();
    CHECK(pr.done == 3);
    CHECK(pr.failed == 1);  // the missing source
    CHECK(ex.takeLog().size() == 3);

    std::string err;
    auto ra = loadImage(pathToU8(dir / "out" / "a_edit.png"), err);
    auto rb = loadImage(pathToU8(dir / "out" / "b_edit.png"), err);
    REQUIRE(ra);
    REQUIRE(rb);
    // Each result has its own source's size, and was inverted.
    CHECK(ra->w == 40);
    CHECK(rb->h == 30);
    CHECK(ra->pixel(0)[0] == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(rb->pixel(0)[0] == doctest::Approx(1.0f).epsilon(0.01));
    fs::remove_all(dir);
}

TEST_CASE("Exporter can be cancelled") {
    Graph g;
    g.addNode("io.output");
    Exporter ex;
    ex.cancel();
    std::vector<ExportItem> items(50, ExportItem{"", pathToU8(fs::temp_directory_path() / "nodelab_never.png")});
    ex.start(g.toJson(), items, 0, ExportSettings{});
    ex.cancel();
    ex.wait();
    CHECK(ex.progress().done < 50);
}
