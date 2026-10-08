#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include "graph/Evaluator.h"
#include "io/ImageCache.h"
#include "io/ImageIO.h"
#include "io/Paths.h"
#include "io/ProjectFile.h"

namespace fs = std::filesystem;

namespace {

// Writes a small deterministic gradient PNG and returns its path.
std::string makeTestImage(const std::string& name) {
    Image img(8, 4);
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            float* p = img.pixel(size_t(y) * img.w + x);
            p[0] = x / 7.0f;
            p[1] = y / 3.0f;
            p[2] = 0.5f;
            p[3] = 1.0f;
        }
    fs::path path = fs::temp_directory_path() / name;
    std::string err;
    REQUIRE(saveImage(pathToU8(path), img, err));
    return pathToU8(path);
}

EvalContext makeCtx(ImageCache& cache, const Graph& g) {
    EvalContext ctx;
    ctx.proxy = false;
    ctx.cache = &cache;
    initContextSize(g, ctx);
    return ctx;
}

}  // namespace

TEST_CASE("value conversions") {
    auto img = std::make_shared<Image>(2, 1);
    float* p = img->pixel(0);
    p[0] = 1; p[1] = 0; p[2] = 0; p[3] = 1;
    ChannelPtr c = toChannel(Value(ImagePtr(img)));
    REQUIRE(c);
    CHECK_FALSE(c->constant);
    CHECK(c->data[0] == doctest::Approx(0.2126f));

    ChannelPtr k = toChannel(Value(0.25f));
    CHECK(k->constant);
    CHECK(k->at(123) == doctest::Approx(0.25f));

    ImagePtr gray = toImage(Value(0.5f), 3, 2);
    REQUIRE(gray);
    CHECK(gray->w == 3);
    CHECK(gray->pixel(5)[1] == doctest::Approx(0.5f));

    CHECK(canConvert(PinType::Image, PinType::Channel));
    CHECK(canConvert(PinType::Number, PinType::Channel));
    CHECK_FALSE(canConvert(PinType::Image, PinType::Number));
}

TEST_CASE("split -> combine is identity") {
    std::string file = makeTestImage("refractory_test_split.png");
    Graph g;
    Node* in = g.addNode("io.image_input");
    in->params[0] = file;
    Node* split = g.addNode("color.split_rgb");
    Node* comb = g.addNode("color.combine_rgb");
    Node* out = g.addNode("io.output");
    for (int k = 0; k < 4; ++k) REQUIRE(g.connect(split->id, k, comb->id, k));
    REQUIRE(g.connect(in->id, 0, split->id, 0));
    REQUIRE(g.connect(comb->id, 0, out->id, 0));

    ImageCache cache;
    EvalContext ctx = makeCtx(cache, g);
    Evaluator ev;
    ImagePtr result = ev.evaluateDisplay(g, out->id, ctx);
    ImagePtr source = cache.get(file, false);
    REQUIRE(result);
    REQUIRE(source);
    REQUIRE(result->px.size() == source->px.size());
    for (size_t i = 0; i < result->px.size(); ++i) CHECK(result->px[i] == doctest::Approx(source->px[i]));
}

TEST_CASE("graph rejects cycles and bad types") {
    Graph g;
    Node* a = g.addNode("color.saturation");
    Node* b = g.addNode("color.invert");
    Node* num = g.addNode("io.number");
    REQUIRE(g.connect(a->id, 0, b->id, 0));
    std::string why;
    CHECK(g.connect(b->id, 0, a->id, 0, &why) == 0);
    CHECK(why == "would create a cycle");
    // Image output into a Number-only input is not allowed; number into channel is.
    CHECK(g.connect(num->id, 0, a->id, 1) != 0);
}

TEST_CASE("evaluator recomputes only dirty nodes") {
    std::string file = makeTestImage("refractory_test_dirty.png");
    Graph g;
    Node* in = g.addNode("io.image_input");
    in->params[0] = file;
    Node* sat = g.addNode("color.saturation");
    Node* inv = g.addNode("color.invert");
    Node* out = g.addNode("io.output");
    g.connect(in->id, 0, sat->id, 0);
    g.connect(sat->id, 0, inv->id, 0);
    g.connect(inv->id, 0, out->id, 0);

    ImageCache cache;
    EvalContext ctx = makeCtx(cache, g);
    Evaluator ev;
    ev.evaluateDisplay(g, out->id, ctx);
    CHECK(ev.recomputeCount == 3);  // input, saturation, invert

    ev.recomputeCount = 0;
    ev.evaluateDisplay(g, out->id, ctx);
    CHECK(ev.recomputeCount == 0);

    inv->params[0] = 0.5f;
    ev.recomputeCount = 0;
    ev.evaluateDisplay(g, out->id, ctx);
    CHECK(ev.recomputeCount == 1);

    sat->params[0] = 2.0f;
    ev.recomputeCount = 0;
    ev.evaluateDisplay(g, out->id, ctx);
    CHECK(ev.recomputeCount == 2);
}

TEST_CASE("channel drives a parameter per pixel") {
    std::string file = makeTestImage("refractory_test_perpixel.png");
    Graph g;
    Node* in = g.addNode("io.image_input");
    in->params[0] = file;
    Node* split = g.addNode("color.split_rgb");
    Node* inv = g.addNode("color.invert");
    Node* out = g.addNode("io.output");
    g.connect(in->id, 0, split->id, 0);
    g.connect(in->id, 0, inv->id, 0);
    g.connect(split->id, 0, inv->id, 1);  // red channel = invert factor
    g.connect(inv->id, 0, out->id, 0);

    ImageCache cache;
    EvalContext ctx = makeCtx(cache, g);
    Evaluator ev;
    ImagePtr r = ev.evaluateDisplay(g, out->id, ctx);
    REQUIRE(r);
    // Left column: red=0 -> factor 0 -> unchanged blue 0.5. Right column: red=1 -> fully inverted red.
    CHECK(r->pixel(0)[2] == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(r->pixel(7)[0] == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("project round-trip") {
    std::string file = makeTestImage("refractory_test_roundtrip.png");
    Graph g;
    Node* in = g.addNode("io.image_input", 10, 20);
    in->params[0] = file;
    Node* bc = g.addNode("color.brightness_contrast", 200, 40);
    bc->params[1] = 0.3f;
    Node* out = g.addNode("io.output", 400, 20);
    g.connect(in->id, 0, bc->id, 0);
    g.connect(bc->id, 0, out->id, 0);

    fs::path proj = fs::temp_directory_path() / "refractory_roundtrip.refract";
    std::string err;
    nlohmann::json ui = {{"zoom", 2.0}};
    REQUIRE(saveProject(pathToU8(proj), g, ui, err));

    Graph g2;
    nlohmann::json ui2;
    REQUIRE(loadProject(pathToU8(proj), g2, ui2, err));
    fs::path base = proj.parent_path();
    CHECK(g.toJson(&base) == g2.toJson(&base));
    CHECK(ui2["zoom"] == 2.0);
    CHECK(fs::equivalent(u8ToPath(g2.find(in->id)->paramS(0)), u8ToPath(file)));
}
