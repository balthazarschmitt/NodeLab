// Lightroom-style develop nodes: Basic, Color Mixer, Color Grading, Lens Correction, Crop's
// straighten/aspect, and the gradient and brush masks.
#include <doctest/doctest.h>

#include <cmath>

#include "graph/Graph.h"
#include "graph/NodeRegistry.h"
#include "nodes/matte/MatteNodes.h"
#include "nodes/transform/TransformNodes.h"

namespace {

// A w x h image with smoothly varying colours covering darks to brights.
ImagePtr testImage(int w = 16, int h = 12) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img->pixel(size_t(y) * w + x);
            p[0] = 0.05f + 0.9f * x / (w - 1);
            p[1] = 0.05f + 0.9f * y / (h - 1);
            p[2] = 0.5f + 0.4f * std::sin(x * 0.7f + y * 0.3f);
            p[3] = 1.0f;
        }
    return img;
}

ImagePtr solid(float r, float g, float b, int w = 8, int h = 8) {
    auto img = std::make_shared<Image>(w, h);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        float* p = img->pixel(i);
        p[0] = r, p[1] = g, p[2] = b, p[3] = 1.0f;
    }
    return img;
}

// Runs a node directly: unconnected pins get their fallback param, as the Evaluator does.
Value run(Node& n, std::vector<Value> in, int w = 16, int h = 12) {
    const NodeInfo& info = n.info();
    in.resize(info.inputs.size());
    for (size_t i = 0; i < info.inputs.size(); ++i)
        if (in[i].empty() && info.inputs[i].fallbackParam >= 0) in[i] = Value(n.paramF(info.inputs[i].fallbackParam));
    std::vector<Value> out(info.outputs.size());
    EvalContext ctx;
    ctx.defaultW = w, ctx.defaultH = h;
    n.evaluate(ctx, in, out);
    return out[0];
}

ImagePtr img(const Value& v) {
    auto p = std::get_if<ImagePtr>(&v.v);
    REQUIRE(p);
    REQUIRE(*p);
    return *p;
}

float maxDiff(const Image& a, const Image& b) {
    REQUIRE(a.w == b.w);
    REQUIRE(a.h == b.h);
    float m = 0;
    for (size_t i = 0; i < a.px.size(); ++i) m = std::max(m, std::fabs(a.px[i] - b.px[i]));
    return m;
}

struct Fixture {
    Graph g;
    Node* add(const char* type) {
        Node* n = g.addNode(type);
        REQUIRE(n);
        return n;
    }
};

}  // namespace

TEST_CASE_FIXTURE(Fixture, "develop nodes at their defaults leave the image unchanged") {
    ImagePtr src = testImage();
    for (const char* t : {"color.basic", "color.color_mixer", "color.color_grading", "xform.lens_correction"}) {
        CAPTURE(t);
        Node* n = add(t);
        CHECK(maxDiff(*img(run(*n, {Value(src)})), *src) < 2e-3f);
    }
}

TEST_CASE_FIXTURE(Fixture, "Basic exposure, white balance and saturation") {
    Node* n = add("color.basic");
    ImagePtr grey = solid(0.5f, 0.5f, 0.5f);
    n->params[3] = 1.0f;  // Exposure +1 stop doubles linear light
    float v = img(run(*n, {Value(grey)}))->pixel(0)[0];
    CHECK(v == doctest::Approx(0.686f).epsilon(0.03));
    n->params[3] = 0.0f;
    n->params[1] = 60.0f;  // warmer
    ImagePtr wImg = img(run(*n, {Value(grey)}));
    const float* w = wImg->pixel(0);
    CHECK(w[0] > w[2] + 0.05f);
    n->params[1] = 0.0f;
    n->params[13] = -100.0f;  // Saturation -100 is greyscale
    ImagePtr sImg = img(run(*n, {Value(solid(0.8f, 0.3f, 0.2f))}));
    const float* s = sImg->pixel(0);
    CHECK(s[0] == doctest::Approx(s[1]).epsilon(0.01));
    CHECK(s[1] == doctest::Approx(s[2]).epsilon(0.01));
}

TEST_CASE_FIXTURE(Fixture, "Color Mixer adjusts only the chosen band") {
    Node* n = add("color.color_mixer");
    n->params[9] = -100.0f;  // Red Saturation
    ImagePtr redImg = img(run(*n, {Value(solid(0.9f, 0.1f, 0.1f))}));
    const float* red = redImg->pixel(0);
    CHECK(red[0] - red[2] < 0.1f);  // nearly grey
    ImagePtr blueImg = img(run(*n, {Value(solid(0.1f, 0.2f, 0.9f))}));
    const float* blue = blueImg->pixel(0);
    CHECK(blue[0] == doctest::Approx(0.1f).epsilon(0.01));
    CHECK(blue[2] == doctest::Approx(0.9f).epsilon(0.01));
}

TEST_CASE_FIXTURE(Fixture, "Color Grading tints shadows but not highlights") {
    Node* n = add("color.color_grading");
    n->params[1] = 220.0f;  // Shadows Hue: blue
    n->params[2] = 100.0f;  // Shadows Saturation
    ImagePtr darkImg = img(run(*n, {Value(solid(0.1f, 0.1f, 0.1f))}));
    const float* dark = darkImg->pixel(0);
    CHECK(dark[2] > dark[0] + 0.02f);
    ImagePtr brightImg = img(run(*n, {Value(solid(0.95f, 0.95f, 0.95f))}));
    const float* bright = brightImg->pixel(0);
    CHECK(std::fabs(bright[2] - bright[0]) < 0.02f);
}

TEST_CASE("crop aspect presets shrink the rectangle around its centre") {
    Graph g;
    Node* n = g.addNode(crop::kType);
    REQUIRE(n);
    n->params[crop::Aspect] = 2;  // 1:1
    crop::Rect rc = crop::effectiveRect(*n, 200, 100);
    CHECK((rc.r - rc.l) * 200 == doctest::Approx((rc.b - rc.t) * 100));
    CHECK((rc.l + rc.r) * 0.5f == doctest::Approx(0.5f));
    CHECK(rc.t == doctest::Approx(0.0f));
    CHECK(crop::aspectRatio(1, 300, 200) == doctest::Approx(1.5f));  // Original
    CHECK(crop::aspectRatio(0, 300, 200) == 0.0f);                   // Free
}

TEST_CASE_FIXTURE(Fixture, "Crop straighten keeps the size and fills the corners") {
    Node* n = add(crop::kType);
    n->params[crop::Angle] = 10.0f;
    ImagePtr src = solid(0.4f, 0.6f, 0.8f, 40, 30);
    ImagePtr out = img(run(*n, {Value(src)}));
    CHECK(out->w == 40);
    CHECK(out->h == 30);
    CHECK(out->pixel(0)[3] == doctest::Approx(1.0f));  // Constrain to Image: no empty corner
    n->params[crop::Constrain] = false;
    CHECK(img(run(*n, {Value(src)}))->pixel(0)[3] < 0.5f);
}

TEST_CASE_FIXTURE(Fixture, "Lens Correction vignetting darkens the corners") {
    Node* n = add("xform.lens_correction");
    n->params[4] = -100.0f;
    ImagePtr out = img(run(*n, {Value(solid(0.6f, 0.6f, 0.6f, 21, 21))}));
    const float centre = out->pixel(10 * 21 + 10)[0], corner = out->pixel(0)[0];
    CHECK(centre == doctest::Approx(0.6f).epsilon(0.02));
    CHECK(corner < 0.45f);
}

TEST_CASE_FIXTURE(Fixture, "Linear Gradient runs from full at Start to none at End") {
    Node* n = add("matte.linear_gradient");
    n->params[0] = 0.5f, n->params[1] = 0.0f, n->params[2] = 0.5f, n->params[3] = 1.0f;
    ChannelPtr c = toChannel(run(*n, {}, 10, 100));
    REQUIRE(c);
    CHECK(c->at(0 * 10 + 5) > 0.95f);
    CHECK(c->at(50 * 10 + 5) == doctest::Approx(0.5f).epsilon(0.05));
    CHECK(c->at(99 * 10 + 5) < 0.05f);
}

TEST_CASE("Brush Mask paints, erases and round-trips its strokes") {
    BrushMaskNode::Stroke paint;
    paint.radius = 0.1f, paint.feather = 0.0f;
    paint.pts = {{0.2f, 0.5f}, {0.8f, 0.5f}};
    std::vector<float> mask(100 * 100, 0.0f);
    paintStrokes(mask, 100, 100, {paint});
    CHECK(mask[50 * 100 + 50] == doctest::Approx(1.0f));
    CHECK(mask[5 * 100 + 50] == 0.0f);
    BrushMaskNode::Stroke erase = paint;
    erase.erase = true;
    erase.pts = {{0.5f, 0.5f}};
    paintStrokes(mask, 100, 100, {erase});
    CHECK(mask[50 * 100 + 50] == doctest::Approx(0.0f));
    CHECK(mask[50 * 100 + 25] == doctest::Approx(1.0f));

    Graph g;
    auto* b = dynamic_cast<BrushMaskNode*>(g.addNode("matte.brush_mask"));
    REQUIRE(b);
    b->strokes = {paint, erase};
    nlohmann::json j;
    b->saveExtra(j);
    Graph g2;
    auto* b2 = dynamic_cast<BrushMaskNode*>(g2.addNode("matte.brush_mask"));
    REQUIRE(b2);
    b2->loadExtra(j);
    REQUIRE(b2->strokes.size() == 2);
    CHECK(b2->strokes[1].erase);
    CHECK(b2->strokes[0].pts.size() == 2);
    CHECK(b2->signatureExtra() == b->signatureExtra());
    b2->strokes.pop_back();
    CHECK(b2->signatureExtra() != b->signatureExtra());
}
