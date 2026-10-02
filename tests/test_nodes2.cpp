#include <doctest/doctest.h>

#include <cmath>

#include "core/ColorMath.h"
#include "graph/Evaluator.h"
#include "graph/NodeRegistry.h"
#include "nodes/ImageOps.h"

namespace {

// A 32x24 gradient image as a constant source node: an Image Expression with no inputs.
Node* gradientSource(Graph& g) {
    Node* e = g.addNode("conv.image_expression");
    e->params[0] = "u";
    e->params[1] = "v";
    e->params[2] = "0.5 + 0.4 * sin(u * 12)";
    return e;
}

EvalContext smallCtx() {
    EvalContext ctx;
    ctx.defaultW = 32;
    ctx.defaultH = 24;
    return ctx;
}

ImagePtr evalImage(Graph& g, int id, int pin = 0) {
    EvalContext ctx = smallCtx();
    Evaluator ev;
    Value v = ev.evaluateOutput(g, id, pin, ctx);
    return toImage(v, ctx.defaultW, ctx.defaultH);
}

ChannelPtr evalChannel(Graph& g, int id, int pin = 0) {
    EvalContext ctx = smallCtx();
    Evaluator ev;
    return toChannel(ev.evaluateOutput(g, id, pin, ctx));
}

float meanOf(const Image& img, int k) {
    double s = 0;
    for (size_t i = 0; i < img.pixelCount(); ++i) s += img.pixel(i)[k];
    return float(s / img.pixelCount());
}

}  // namespace

TEST_CASE("every node evaluates with image and channel inputs connected") {
    for (const auto& type : NodeRegistry::instance().types()) {
        const NodeInfo* inf = NodeRegistry::instance().find(type);
        if (inf->hidden) continue;
        CAPTURE(type);
        Graph g;
        Node* src = gradientSource(g);
        Node* split = g.addNode("color.split_rgb");
        g.connect(src->id, 0, split->id, 0);
        Node* n = g.addNode(type);
        REQUIRE(n);
        for (int i = 0; i < int(n->info().inputs.size()); ++i) {
            PinType t = n->info().inputs[i].type;
            if (t == PinType::Image) g.connect(src->id, 0, n->id, i);
            else if (t == PinType::Channel) g.connect(split->id, 1, n->id, i);  // green = v gradient
        }
        EvalContext ctx = smallCtx();
        Evaluator ev;
        if (n->info().outputs.empty()) {
            CHECK_NOTHROW(ev.evaluateDisplay(g, n->id, ctx));
            continue;
        }
        for (int o = 0; o < int(n->info().outputs.size()); ++o) {
            Value v;
            CHECK_NOTHROW(v = ev.evaluateOutput(g, n->id, o, ctx));
            if (type != "io.image_input") CHECK_FALSE(v.empty());  // no file chosen -> nothing
        }
    }
}

TEST_CASE("wavelength and blackbody colors") {
    float r, g, b;
    colormath::wavelengthToRgb(450, r, g, b);
    CHECK(b > r);
    CHECK(b > g);
    colormath::wavelengthToRgb(550, r, g, b);
    CHECK(g > r * 0.9f);
    CHECK(g > b);
    colormath::wavelengthToRgb(650, r, g, b);
    CHECK(r > g);
    CHECK(r > b);
    colormath::wavelengthToRgb(900, r, g, b);  // infrared: invisible
    CHECK(r + g + b < 0.05f);

    colormath::blackbodyToRgb(1900, r, g, b);  // candle: orange
    CHECK(r == doctest::Approx(1.0f));
    CHECK(r > g);
    CHECK(g > b);
    colormath::blackbodyToRgb(12000, r, g, b);  // hot: bluish
    CHECK(b >= r);
}

TEST_CASE("color space round trips (YCbCr, YUV, HSL)") {
    using namespace colormath;
    const float s[3] = {0.7f, 0.2f, 0.45f};
    float a, b2, c, r, g, b;
    rgbToYCbCr(s[0], s[1], s[2], a, b2, c);
    yCbCrToRgb(a, b2, c, r, g, b);
    CHECK(r == doctest::Approx(s[0]).epsilon(1e-4));
    CHECK(g == doctest::Approx(s[1]).epsilon(1e-4));
    rgbToYuv(s[0], s[1], s[2], a, b2, c);
    yuvToRgb(a, b2, c, r, g, b);
    CHECK(b == doctest::Approx(s[2]).epsilon(1e-2));
    rgbToHsl(s[0], s[1], s[2], a, b2, c);
    hslToRgb(a, b2, c, r, g, b);
    CHECK(r == doctest::Approx(s[0]).epsilon(1e-4));
    CHECK(b == doctest::Approx(s[2]).epsilon(1e-4));
}

TEST_CASE("blur preserves a flat image and the mean") {
    Image flat(40, 30);
    for (size_t i = 0; i < flat.pixelCount(); ++i) flat.pixel(i)[0] = 0.3f;
    imageops::blurImage(flat, 6, 6);
    CHECK(flat.pixel(0)[0] == doctest::Approx(0.3f));
    CHECK(flat.pixel(599)[0] == doctest::Approx(0.3f));

    Graph g;
    Node* src = gradientSource(g);
    Node* blur = g.addNode("filter.blur");
    g.connect(src->id, 0, blur->id, 0);
    ImagePtr a = evalImage(g, src->id), b = evalImage(g, blur->id);
    CHECK(meanOf(*b, 2) == doctest::Approx(meanOf(*a, 2)).epsilon(0.02));
}

TEST_CASE("transform identity and flip twice") {
    Graph g;
    Node* src = gradientSource(g);
    Node* t = g.addNode("xform.transform");
    Node* f1 = g.addNode("xform.flip");
    Node* f2 = g.addNode("xform.flip");
    g.connect(src->id, 0, t->id, 0);
    g.connect(src->id, 0, f1->id, 0);
    g.connect(f1->id, 0, f2->id, 0);
    ImagePtr a = evalImage(g, src->id), bt = evalImage(g, t->id), bf = evalImage(g, f2->id);
    for (size_t i = 0; i < a->px.size(); i += 37) {
        CHECK(bt->px[i] == doctest::Approx(a->px[i]).epsilon(1e-4));
        CHECK(bf->px[i] == doctest::Approx(a->px[i]));
    }
}

TEST_CASE("distance transform and dilate") {
    std::vector<uint8_t> m(21 * 21, 0);
    m[10 * 21 + 10] = 1;
    auto d = imageops::distanceTransform(m, 21, 21);
    CHECK(d[10 * 21 + 10] == doctest::Approx(0.0f));
    CHECK(d[10 * 21 + 13] == doctest::Approx(3.0f));
    CHECK(d[14 * 21 + 13] == doctest::Approx(5.0f));  // 3-4-5 triangle

    // Dilate a centered box mask: area grows.
    Graph g;
    Node* box = g.addNode("matte.box_mask");
    box->params[5] = 0.0f;  // no feather
    Node* dil = g.addNode("filter.dilate_erode");
    dil->params[1] = 3.0f;
    g.connect(box->id, 0, dil->id, 0);
    ChannelPtr a = evalChannel(g, box->id), b = evalChannel(g, dil->id);
    double sa = 0, sb = 0;
    for (size_t i = 0; i < a->data.size(); ++i) sa += a->data[i], sb += b->data[i];
    CHECK(sb > sa);
    CHECK(a->at(12 * 32 + 16) == doctest::Approx(1.0f));  // center inside
    CHECK(a->at(0) == doctest::Approx(0.0f));             // corner outside
}

TEST_CASE("neutral grading nodes are identities") {
    Graph g;
    Node* src = gradientSource(g);
    for (const char* type : {"color.hue_correct", "color.color_balance"}) {
        CAPTURE(type);
        Node* n = g.addNode(type);
        g.connect(src->id, 0, n->id, 0);
        ImagePtr a = evalImage(g, src->id), b = evalImage(g, n->id);
        for (size_t i = 0; i < a->px.size(); i += 29) CHECK(b->px[i] == doctest::Approx(a->px[i]).epsilon(2e-3));
    }
}

TEST_CASE("noise is deterministic and in range") {
    Graph g;
    Node* n = g.addNode("tex.noise");
    ChannelPtr a = evalChannel(g, n->id), b = evalChannel(g, n->id);
    REQUIRE(a);
    for (size_t i = 0; i < a->data.size(); ++i) {
        CHECK(a->data[i] == b->data[i]);
        CHECK(a->data[i] >= -0.05f);
        CHECK(a->data[i] <= 1.05f);
    }
}

TEST_CASE("Fast directional and bilateral blurs stay close to High quality") {
    auto meanDiff = [](const Image& a, const Image& b) {
        double s = 0;
        for (size_t i = 0; i < a.pixelCount(); ++i)
            for (int k = 0; k < 3; ++k) s += std::fabs(a.pixel(i)[k] - b.pixel(i)[k]);
        return float(s / (a.pixelCount() * 3));
    };
    struct Case {
        const char* type;
        std::vector<std::pair<int, float>> params;
        int quality;
    };
    for (const Case& c : {Case{"filter.directional_blur", {{0, 6.0f}}, 6}, Case{"filter.directional_blur", {{0, 9.0f}, {1, 40.0f}, {2, 5.0f}}, 6},
                          Case{"filter.bilateral_blur", {{0, 4.0f}, {1, 0.3f}}, 2}}) {
        CAPTURE(c.type);
        Graph g;
        Node* src = gradientSource(g);
        Node* hi = g.addNode(c.type);
        Node* lo = g.addNode(c.type);
        for (auto [i, v] : c.params) hi->params[size_t(i)] = v, lo->params[size_t(i)] = v;
        lo->params[size_t(c.quality)] = 1;
        g.connect(src->id, 0, hi->id, 0);
        g.connect(src->id, 0, lo->id, 0);
        ImagePtr s = evalImage(g, src->id), a = evalImage(g, hi->id), b = evalImage(g, lo->id);
        CHECK(meanDiff(*a, *s) > 0.002f);  // it does blur
        CHECK(meanDiff(*a, *b) < 0.25f * meanDiff(*a, *s));
    }
}
