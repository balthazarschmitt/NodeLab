// Scene-linear colour pipeline: sRGB decoding on load, unclamped Exposure, the view transforms,
// and legacy projects staying legacy.
#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>

#include "core/ColorManagement.h"
#include "core/ColorMath.h"
#include "graph/Graph.h"
#include "graph/NodeRegistry.h"
#include "io/ImageIO.h"
#include "io/ProjectFile.h"
#include "nodes/io/IONodes.h"

namespace fs = std::filesystem;

namespace {

float view(const ColorManagement& cm, float v) {
    const float in[3] = {v, v, v};
    float out[3];
    colormgmt::viewTransform(cm, in, out);
    return out[1];
}

}  // namespace

TEST_CASE("sRGB images decode to linear light on load, alpha untouched") {
    Image img(4, 1);
    const float levels[4] = {0.0f, 0.2f, 0.5f, 1.0f};
    for (int x = 0; x < 4; ++x) {
        float* p = img.pixel(size_t(x));
        p[0] = p[1] = p[2] = levels[x];
        p[3] = 0.5f;
    }
    const fs::path path = fs::temp_directory_path() / "nodelab_colorpipe.png";
    std::string err;
    REQUIRE(saveImage(path.string(), img, err));
    auto raw = loadImage(path.string(), err, false);
    auto lin = loadImage(path.string(), err, true);
    REQUIRE(raw);
    REQUIRE(lin);
    for (int x = 0; x < 4; ++x) {
        CAPTURE(x);
        const float* r = raw->pixel(size_t(x));
        const float* l = lin->pixel(size_t(x));
        CHECK(l[0] == doctest::Approx(colormath::srgbToLinear(r[0])).epsilon(1e-6));
        CHECK(l[3] == r[3]);  // alpha is never linearised
    }
    fs::remove(path);
}

TEST_CASE("Exposure is an unclamped multiply in scene-linear projects") {
    Graph g;
    Node* n = g.addNode("color.exposure");
    REQUIRE(n);
    auto img = std::make_shared<Image>(1, 1);
    float* p = img->pixel(0);
    p[0] = 0.3f, p[1] = 0.8f, p[2] = 3.0f, p[3] = 1.0f;
    std::vector<Value> in = {Value(ImagePtr(img)), Value(1.0f)}, out(1);  // +1 stop
    EvalContext ctx;
    ctx.defaultW = ctx.defaultH = 1;
    ctx.colorManagement = ColorManagement::sceneLinear();
    n->evaluate(ctx, in, out);
    ImagePtr r = toImage(out[0], 0, 0);
    REQUIRE(r);
    CHECK(r->pixel(0)[0] == doctest::Approx(0.6f));
    CHECK(r->pixel(0)[1] == doctest::Approx(1.6f));
    CHECK(r->pixel(0)[2] == doctest::Approx(6.0f));

    ctx.colorManagement = ColorManagement();  // legacy: encoded values, clamped to 1
    n->evaluate(ctx, in, out);
    r = toImage(out[0], 0, 0);
    CHECK(r->pixel(0)[1] == doctest::Approx(1.0f));
}

TEST_CASE("Standard view round-trips display values and Raw passes them through") {
    ColorManagement cm = ColorManagement::sceneLinear();
    for (int i = 0; i <= 255; ++i) {
        const float enc = i / 255.0f;
        CHECK(std::abs(view(cm, colormath::srgbToLinear(enc)) - enc) < 0.5f / 255.0f);
    }
    CHECK(view(cm, 4.0f) == doctest::Approx(1.0f));  // Standard clips
    cm.exposure = 1.0f;
    CHECK(view(cm, 0.1f) == doctest::Approx(colormath::linearToSrgb(0.2f)));
    cm = ColorManagement::sceneLinear();
    cm.view = ColorManagement::Raw;
    CHECK(view(cm, 0.25f) == doctest::Approx(0.25f));
}

TEST_CASE("AgX is monotonic, keeps greys neutral and rolls highlights off") {
    ColorManagement cm = ColorManagement::sceneLinear();
    cm.view = ColorManagement::AgX;
    float prev = -1.0f;
    for (float ev = -10.0f; ev <= 6.0f; ev += 0.25f) {
        const float in[3] = {0.18f * std::exp2(ev), 0.18f * std::exp2(ev), 0.18f * std::exp2(ev)};
        float out[3];
        colormgmt::viewTransform(cm, in, out);
        CAPTURE(ev);
        CHECK(out[0] >= prev);
        CHECK(std::abs(out[0] - out[1]) < 2e-3f);
        CHECK(std::abs(out[2] - out[1]) < 2e-3f);
        prev = out[1];
    }
    const float grey = view(cm, 0.18f);
    CHECK(grey > 0.4f);
    CHECK(grey < 0.6f);
    CHECK(view(cm, 8.0f) < 1.0f);    // 5.5 stops over still isn't clipped...
    CHECK(view(cm, 8.0f) > 0.9f);    // ...but it is nearly white
    CHECK(view(cm, 100.0f) <= 1.0f);

    // A saturated light desaturates toward white instead of clipping to a flat primary.
    const float red[3] = {20.0f, 0.5f, 0.5f};
    float out[3];
    colormgmt::viewTransform(cm, red, out);
    CHECK(out[1] > 0.3f);

    cm.look = ColorManagement::Greyscale;
    colormgmt::viewTransform(cm, red, out);
    CHECK(std::abs(out[0] - out[1]) < 2e-3f);
}

TEST_CASE("Legacy graphs have no colour management block; scene-linear ones round-trip") {
    Graph g;
    g.addNode("color.exposure");
    CHECK_FALSE(g.toJson().contains("colorManagement"));
    Graph back;
    back.fromJson(g.toJson());
    CHECK_FALSE(back.colorManagement.linear);

    g.colorManagement = ColorManagement::sceneLinear();
    g.colorManagement.view = ColorManagement::AgX;
    g.colorManagement.look = ColorManagement::Punchy;
    g.colorManagement.exposure = -0.5f;
    back.fromJson(g.toJson());
    CHECK(back.colorManagement == g.colorManagement);
}

TEST_CASE("Only scene-linear projects need the newer project format") {
    Graph g;
    g.addNode("color.exposure");
    const fs::path path = fs::temp_directory_path() / "nodelab_colorpipe.nlproj";
    auto savedVersion = [&] {
        std::string err;
        REQUIRE(saveProject(path.string(), g, nlohmann::json::object(), err));
        std::ifstream f(path);
        return nlohmann::json::parse(f).value("version", 0);
    };
    CHECK(savedVersion() == 1);  // older builds can still open legacy projects
    g.colorManagement = ColorManagement::sceneLinear();
    CHECK(savedVersion() == kProjectVersion);
    Graph back;
    nlohmann::json ui;
    std::string err;
    REQUIRE(loadProject(path.string(), back, ui, err));
    CHECK(back.colorManagement.linear);
    fs::remove(path);
}

TEST_CASE("Image Input linearises sRGB files only in scene-linear projects") {
    Graph g;
    Node* n = g.addNode("io.image_input");
    REQUIRE(n);
    const auto& in = static_cast<const ImageInputNode&>(*n);
    CHECK_FALSE(in.decode(false).srgbToLinear);
    CHECK_FALSE(in.decode(false).sceneLinear);
    CHECK(in.decode(true).srgbToLinear);
    CHECK(in.decode(true).sceneLinear);
    n->params[1] = 1;  // Linear Rec.709
    CHECK_FALSE(in.decode(true).srgbToLinear);
    n->params[1] = 2;  // Non-Color
    CHECK_FALSE(in.decode(true).srgbToLinear);
    n->params[2] = 2;  // Reconstruct highlights
    CHECK(in.decode(true).rawHighlights == 2);
}

TEST_CASE("Contrast pivots on middle grey and never goes negative in scene-linear projects") {
    Graph g;
    Node* n = g.addNode("color.brightness_contrast");
    REQUIRE(n);
    auto img = std::make_shared<Image>(1, 1);
    float* p = img->pixel(0);
    p[0] = 0.18f, p[1] = 0.01f, p[2] = 4.0f, p[3] = 1.0f;
    std::vector<Value> in = {Value(ImagePtr(img)), Value(0.0f), Value(0.5f)}, out(1);
    EvalContext ctx;
    ctx.defaultW = ctx.defaultH = 1;
    ctx.colorManagement = ColorManagement::sceneLinear();
    n->evaluate(ctx, in, out);
    ImagePtr r = toImage(out[0], 0, 0);
    REQUIRE(r);
    CHECK(r->pixel(0)[0] == doctest::Approx(0.18f));
    CHECK(r->pixel(0)[1] > 0.0f);
    CHECK(r->pixel(0)[1] < 0.01f);
    CHECK(r->pixel(0)[2] > 4.0f);  // unclamped highlights
}

TEST_CASE("displayImage matches viewTransform for every view, look, exposure and gamma") {
    // displayImage uses tables; viewTransform is the exact reference.
    auto img = std::make_shared<Image>(256, 64);
    uint32_t seed = 12345;
    auto rnd = [&] {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / float(1u << 24);
    };
    for (size_t i = 0; i < img->pixelCount(); ++i) {
        float* p = img->pixel(i);
        for (int c = 0; c < 3; ++c) {
            // Over 20 stops, with some negatives, zeros and very bright values.
            const float r = rnd();
            p[c] = r < 0.05f ? -rnd() : r < 0.1f ? 0.0f : std::exp2(rnd() * 22.0f - 16.0f);
        }
        p[3] = rnd();
    }
    // Worst difference for Standard and Raw, and for AgX.
    float worst[2] = {};
    for (int view = 0; view < 3; ++view)
        for (int look = 0; look < 3; ++look)
            for (float exposure : {0.0f, -1.3f, 2.0f})
                for (float gamma : {1.0f, 1.8f}) {
                    ColorManagement cm = ColorManagement::sceneLinear();
                    cm.view = view, cm.look = look, cm.exposure = exposure, cm.gamma = gamma;
                    const ImagePtr d = colormgmt::displayImage(img, cm);
                    float& w = worst[view == ColorManagement::AgX];
                    for (size_t i = 0; i < img->pixelCount(); ++i) {
                        float want[3];
                        colormgmt::viewTransform(cm, img->pixel(i), want);
                        // Gamma steepens near black: compare before it.
                        for (int c = 0; c < 3; ++c)
                            w = std::max(w, std::abs(std::pow(d->pixel(i)[c], gamma) - std::pow(want[c], gamma)));
                        REQUIRE(d->pixel(i)[3] == img->pixel(i)[3]);
                    }
                }
    INFO("largest differences " << worst[0] << ", AgX " << worst[1]);
    CHECK(worst[0] < 4e-6f);  // a 16-bit step is 1.5e-5
    // viewTransform's float AgX polynomial is itself off by up to about 3e-5 near black, where
    // the outset's colours cancel; the tables are computed in double.
    CHECK(worst[1] < 1e-4f);
}
