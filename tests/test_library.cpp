// The photo library's model: folder listing, sidecars, ratings, the default graph, copy/paste.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "graph/Graph.h"
#include "io/Export.h"
#include "io/ImageIO.h"
#include "io/Library.h"
#include "io/Paths.h"
#include "io/ProjectFile.h"
#include "nodes/io/IONodes.h"

namespace fs = std::filesystem;

namespace {

// A scratch folder with a few small photos, removed afterwards.
struct Folder {
    fs::path dir;
    explicit Folder(const char* name) {
        dir = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
    }
    ~Folder() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    std::string photo(const char* name, float r = 0.5f) const {
        Image img(16, 12);
        for (size_t i = 0; i < img.px.size(); i += 4) img.px[i] = r, img.px[i + 1] = 0.3f, img.px[i + 2] = 0.2f, img.px[i + 3] = 1;
        const std::string p = pathToU8(dir / name);
        std::string err;
        REQUIRE(saveImage(p, img, err));
        return p;
    }
};

const Node* firstOf(const Graph& g, const std::string& type) { return g.find(g.firstOfType(type)); }

}  // namespace

TEST_CASE("Library lists a folder's images by name, without sidecars") {
    Folder f("nodelab_lib_list");
    const std::string b = f.photo("b.png"), a = f.photo("A.png"), c = f.photo("c.jpg");
    std::ofstream(f.dir / "notes.txt") << "x";
    std::string err;
    REQUIRE(library::writeMeta(b, library::Meta{3}, err));  // makes b.png.nlproj
    const auto list = library::listFolder(pathToU8(f.dir));
    REQUIRE(list.size() == 3);
    CHECK(list[0] == a);
    CHECK(list[1] == b);
    CHECK(list[2] == c);
    CHECK(library::sidecarPath(b) == b + ".nlproj");
}

TEST_CASE("Library meta round-trips through the sidecar and keeps its graph") {
    Folder f("nodelab_lib_meta");
    const std::string p = f.photo("p.png");
    library::Meta m;
    CHECK_FALSE(library::readMeta(p, m));
    CHECK_FALSE(library::hasSidecar(p));

    // Rating a photo without a sidecar creates one with the default graph.
    m.rating = 4;
    m.flag = library::Picked;
    Image small(8, 6);
    std::fill(small.px.begin(), small.px.end(), 0.5f);
    m.thumb = library::encodeThumb(small);
    std::string err;
    REQUIRE(library::writeMeta(p, m, err));
    library::Meta back;
    REQUIRE(library::readMeta(p, back));
    CHECK(back.rating == 4);
    CHECK(back.flag == library::Picked);
    CHECK_FALSE(back.edited);
    ImagePtr t = library::decodeThumb(back.thumb);
    REQUIRE(t);
    CHECK(t->w == 8);
    CHECK(t->pixel(0)[0] == doctest::Approx(0.5f).epsilon(0.02));

    Graph g;
    nlohmann::json ui;
    REQUIRE(loadProject(library::sidecarPath(p), g, ui, err));
    CHECK(firstOf(g, "filter.denoise"));
    CHECK(firstOf(g, ImageInputNode::staticInfo().type)->paramS(0) == p);

    // Changing the meta again keeps the graph as saved (here: a node added).
    g.addNode("color.invert", 0, 300);
    REQUIRE(saveProject(library::sidecarPath(p), g, ui, err));
    back.flag = library::Rejected;
    REQUIRE(library::writeMeta(p, back, err));
    Graph g2;
    REQUIRE(loadProject(library::sidecarPath(p), g2, ui, err));
    CHECK(firstOf(g2, "color.invert"));
    CHECK(library::Meta::fromJson(ui["library"]).flag == library::Rejected);
}

TEST_CASE("Library default graph: Denoise and Basic, with RAW defaults for RAWs") {
    Graph g;
    library::defaultGraph(g, "C:/photos/a.jpg");
    CHECK(g.colorManagement.linear);
    CHECK(g.colorManagement.view == ColorManagement::Standard);
    const Node* dn = firstOf(g, "filter.denoise");
    REQUIRE(dn);
    CHECK(dn->paramF(2) == 0.0f);  // Color: off for JPEGs
    REQUIRE(firstOf(g, "color.basic"));
    const int out = g.firstOfType(OutputNode::staticInfo().type);
    REQUIRE(g.inputLink(out, 0));
    CHECK(g.inputLink(out, 0)->fromNode == firstOf(g, "color.basic")->id);

    library::defaultGraph(g, "C:/photos/a.CR3");
    CHECK(firstOf(g, "filter.denoise")->paramF(2) == 25.0f);
    CHECK(g.colorManagement.view == ColorManagement::AgX);
    CHECK(firstOf(g, ImageInputNode::staticInfo().type)->paramB(4));  // Baseline Exposure on
}

TEST_CASE("Paste edit points the edit at each photo and keeps its rating") {
    Folder f("nodelab_lib_paste");
    const std::string src = f.photo("src.png"), dst = f.photo("dst.png", 0.9f);
    Graph g;
    library::defaultGraph(g, src);
    Node* inv = g.addNode("color.invert", 0, 300);
    inv->label = "pasted";
    library::Meta m;
    m.rating = 2;
    m.thumb = "AAAA";
    std::string err;
    REQUIRE(library::writeMeta(dst, m, err));

    REQUIRE(library::pasteEdit(g.toJson(), src, dst, err));
    Graph out;
    nlohmann::json ui;
    REQUIRE(loadProject(library::sidecarPath(dst), out, ui, err));
    CHECK(firstOf(out, ImageInputNode::staticInfo().type)->paramS(0) == dst);
    CHECK(firstOf(out, "color.invert")->label == "pasted");
    const library::Meta back = library::Meta::fromJson(ui["library"]);
    CHECK(back.rating == 2);
    CHECK(back.edited);
    CHECK(back.thumb.empty());  // showed the old edit

    // The edit's graph for exporting, with absolute paths.
    const nlohmann::json gj = library::graphFor(dst, err);
    REQUIRE(gj.is_object());
    // A photo without a sidecar exports with the default graph.
    CHECK(library::graphFor(src, err).is_object());
}

TEST_CASE("Library thumbnails: the photo, and the edit rendered through the view") {
    Folder f("nodelab_lib_thumb");
    const std::string p = f.photo("t.png", 1.0f);
    std::string err;
    ImagePtr t = library::loadThumbnail(p, 8, err);
    REQUIRE(t);
    CHECK(std::max(t->w, t->h) == 8);
    CHECK(t->pixel(0)[0] == doctest::Approx(1.0f).epsilon(0.01));

    Graph g;
    library::defaultGraph(g, p);
    Node* inv = g.addNode("color.invert", 0, 300);
    const int out = g.firstOfType(OutputNode::staticInfo().type);
    g.connect(g.inputLink(out, 0)->fromNode, 0, inv->id, 0);
    g.connect(inv->id, 0, out, 0);
    ImagePtr r = library::renderThumbnail(g, 8, err);
    REQUIRE(r);
    CHECK(std::max(r->w, r->h) == 8);
    CHECK(r->pixel(0)[0] < 0.1f);  // inverted red
}

TEST_CASE("base64 round-trips") {
    for (size_t n : {0, 1, 2, 3, 4, 5, 100}) {
        std::vector<unsigned char> v(n);
        for (size_t i = 0; i < n; ++i) v[i] = static_cast<unsigned char>(i * 37 + 11);
        CHECK(library::base64Decode(library::base64Encode(v)) == v);
    }
    CHECK(library::base64Encode({'M', 'a', 'n'}) == "TWFu");
}

TEST_CASE("Export Selected renders each photo with its own edit") {
    Folder f("nodelab_lib_export");
    const std::string a = f.photo("a.png", 0.2f), b = f.photo("b.png", 0.2f);
    // b's edit inverts; a has no sidecar (the default graph).
    Graph g;
    library::defaultGraph(g, b);
    Node* inv = g.addNode("color.invert", 0, 300);
    const int out = g.firstOfType(OutputNode::staticInfo().type);
    g.connect(g.inputLink(out, 0)->fromNode, 0, inv->id, 0);
    g.connect(inv->id, 0, out, 0);
    std::string err;
    REQUIRE(library::pasteEdit(g.toJson(), b, b, err));

    ExportSettings s;
    std::vector<ExportItem> items;
    for (const std::string& p : {a, b}) {
        ExportItem it;
        it.source = p;
        it.output = batchOutputPath(p, pathToU8(f.dir / "out"), s);
        it.graph = library::graphFor(p, err);
        items.push_back(std::move(it));
    }
    fs::create_directories(f.dir / "out");
    Exporter ex;
    ex.start(nullptr, items, 0, s);
    ex.wait();
    CHECK(ex.progress().failed == 0);
    ImagePtr ra = loadImage(items[0].output, err), rb = loadImage(items[1].output, err);
    REQUIRE(ra);
    REQUIRE(rb);
    CHECK(ra->pixel(0)[0] == doctest::Approx(0.2f).epsilon(0.02));
    CHECK(rb->pixel(0)[0] > 0.9f);  // inverted (in linear light)
}
