#include <doctest/doctest.h>

#include <cmath>

#include "core/ColorMath.h"
#include "core/Curve.h"
#include "core/Ramp.h"
#include "graph/Evaluator.h"
#include "graph/NodeRegistry.h"

namespace {

// Evaluates output pin `pin` of node `id` with a small context (no file inputs needed).
Value evalOut(Graph& g, int id, int pin = 0) {
    EvalContext ctx;
    ctx.defaultW = 4;
    ctx.defaultH = 2;
    Evaluator ev;
    return ev.evaluateOutput(g, id, pin, ctx);
}

// A 4x2 image built with Combine RGB from constant channels.
Node* constImage(Graph& g, float r, float gr, float b) {
    Node* c = g.addNode("color.combine_rgb");
    c->params[0] = r;
    c->params[1] = gr;
    c->params[2] = b;
    return c;
}

const float* px(const Value& v, size_t i = 0) {
    auto p = std::get_if<ImagePtr>(&v.v);
    REQUIRE(p);
    REQUIRE(*p);
    return (*p)->pixel(i);
}

float ch(const Value& v, size_t i = 0) {
    ChannelPtr c = toChannel(v);
    REQUIRE(c);
    return c->at(i);
}

}  // namespace

TEST_CASE("color space round trips") {
    using namespace colormath;
    const float samples[][3] = {{0.2f, 0.5f, 0.8f}, {1, 0, 0}, {0.9f, 0.9f, 0.1f}, {0.3f, 0.3f, 0.3f}};
    for (const auto& s : samples) {
        float h, sa, v, r, g, b;
        rgbToHsv(s[0], s[1], s[2], h, sa, v);
        hsvToRgb(h, sa, v, r, g, b);
        CHECK(r == doctest::Approx(s[0]).epsilon(1e-4));
        CHECK(g == doctest::Approx(s[1]).epsilon(1e-4));
        CHECK(b == doctest::Approx(s[2]).epsilon(1e-4));
        float L, A, B;
        rgbToLab(s[0], s[1], s[2], L, A, B);
        labToRgb(L, A, B, r, g, b);
        CHECK(r == doctest::Approx(s[0]).epsilon(1e-3));
        CHECK(g == doctest::Approx(s[1]).epsilon(1e-3));
        CHECK(b == doctest::Approx(s[2]).epsilon(1e-3));
    }
}

TEST_CASE("curves are monotone and pass through points") {
    CurvePoints pts = {{0, 0}, {0.25f, 0.6f}, {0.5f, 0.65f}, {1, 1}};
    CHECK(evalCurve(pts, 0.25f) == doctest::Approx(0.6f));
    CHECK(evalCurve(pts, 0.5f) == doctest::Approx(0.65f));
    float prev = -1;
    for (int i = 0; i <= 100; ++i) {
        float y = evalCurve(pts, i / 100.0f);
        CHECK(y >= prev - 1e-6f);  // Fritsch-Carlson never overshoots on monotone data
        prev = y;
    }
    CHECK(isIdentityCurve(identityCurve()));
}

TEST_CASE("ramp interpolation") {
    ColorRamp r = rampFromJson(nlohmann::json{{"interp", 0}, {"stops", {{0.0, 1, 0, 0, 1}, {1.0, 0, 0, 1, 1}}}});
    float c[4];
    r.eval(0.5f, c);
    CHECK(c[0] == doctest::Approx(0.5f));
    CHECK(c[2] == doctest::Approx(0.5f));
    r.interp = ColorRamp::Constant;
    r.eval(0.99f, c);
    CHECK(c[0] == doctest::Approx(1.0f));
}

TEST_CASE("math node, constant path") {
    Graph g;
    Node* m = g.addNode("conv.math");
    m->params[0] = 3.0f;
    m->params[1] = 2.0f;
    m->params[2] = 4;  // Power
    Value v = evalOut(g, m->id);
    ChannelPtr c = toChannel(v);
    REQUIRE(c);
    CHECK(c->constant);  // no sized inputs -> scalar result
    CHECK(c->value == doctest::Approx(9.0f));
}

TEST_CASE("map range and threshold") {
    Graph g;
    Node* mr = g.addNode("conv.map_range");
    mr->params[0] = 5.0f;  // value
    mr->params[1] = 0.0f;
    mr->params[2] = 10.0f;
    mr->params[3] = 100.0f;
    mr->params[4] = 200.0f;
    CHECK(ch(evalOut(g, mr->id)) == doctest::Approx(150.0f));

    Node* th = g.addNode("conv.threshold");
    th->params[0] = 0.7f;
    th->params[1] = 0.5f;
    CHECK(ch(evalOut(g, th->id)) == doctest::Approx(1.0f));
}

TEST_CASE("blend multiply and screen") {
    Graph g;
    Node* a = constImage(g, 0.5f, 0.5f, 0.5f);
    Node* b = constImage(g, 0.5f, 1.0f, 0.0f);
    Node* bl = g.addNode("math.blend");
    g.connect(a->id, 0, bl->id, 0);
    g.connect(b->id, 0, bl->id, 1);
    bl->params[1] = 2;  // Multiply
    const float* p = px(evalOut(g, bl->id));
    CHECK(p[0] == doctest::Approx(0.25f));
    CHECK(p[1] == doctest::Approx(0.5f));
    CHECK(p[2] == doctest::Approx(0.0f));
    bl->params[1] = 5;  // Screen
    p = px(evalOut(g, bl->id));
    CHECK(p[0] == doctest::Approx(0.75f));
}

TEST_CASE("hsv split/combine is identity") {
    Graph g;
    Node* src = constImage(g, 0.2f, 0.6f, 0.9f);
    Node* sp = g.addNode("color.split_hsv");
    Node* co = g.addNode("color.combine_hsv");
    g.connect(src->id, 0, sp->id, 0);
    for (int k = 0; k < 4; ++k) g.connect(sp->id, k, co->id, k);
    const float* p = px(evalOut(g, co->id));
    CHECK(p[0] == doctest::Approx(0.2f).epsilon(1e-4));
    CHECK(p[1] == doctest::Approx(0.6f).epsilon(1e-4));
    CHECK(p[2] == doctest::Approx(0.9f).epsilon(1e-4));
}

TEST_CASE("color ramp maps a factor") {
    Graph g;
    Node* ramp = g.addNode("conv.color_ramp");
    ramp->params[0] = 0.25f;
    ramp->params[1] = nlohmann::json{{"interp", 0}, {"stops", {{0.0, 0, 0, 0, 1}, {1.0, 1, 0.5, 0, 1}}}};
    const float* p = px(evalOut(g, ramp->id));
    CHECK(p[0] == doctest::Approx(0.25f));
    CHECK(p[1] == doctest::Approx(0.125f));
}

TEST_CASE("color key isolates a hue") {
    Graph g;
    Node* green = constImage(g, 0.1f, 0.8f, 0.1f);
    Node* red = constImage(g, 0.8f, 0.1f, 0.1f);
    Node* k1 = g.addNode("conv.color_key");  // default: hue 120 (green)
    Node* k2 = g.addNode("conv.color_key");
    g.connect(green->id, 0, k1->id, 0);
    g.connect(red->id, 0, k2->id, 0);
    CHECK(ch(evalOut(g, k1->id)) == doctest::Approx(1.0f));
    CHECK(ch(evalOut(g, k2->id)) == doctest::Approx(0.0f));
}

TEST_CASE("expressions") {
    Graph g;
    Node* src = constImage(g, 0.2f, 0.4f, 0.6f);
    Node* e = g.addNode("conv.expression");
    g.connect(src->id, 0, e->id, 0);
    e->params[0] = "max(r, b) * 2 + clamp(g, 0, 0.1)";
    CHECK(ch(evalOut(g, e->id)) == doctest::Approx(1.3f).epsilon(1e-4));

    Node* ie = g.addNode("conv.image_expression");
    g.connect(src->id, 0, ie->id, 0);
    ie->params[0] = "b";
    ie->params[1] = "u";  // u = (x + 0.5) / w
    ie->params[2] = "r";
    const float* p = px(evalOut(g, ie->id), 1);  // pixel x=1 of a 4-wide image
    CHECK(p[0] == doctest::Approx(0.6f));
    CHECK(p[1] == doctest::Approx(0.375f));

    e->params[0] = "r +* 2";
    CHECK_THROWS_WITH_AS(evalOut(g, e->id), doctest::Contains("syntax error"), std::runtime_error);
}

TEST_CASE("every registered node evaluates with no inputs") {
    // Smoke test: unconnected nodes must not crash.
    for (const auto& type : NodeRegistry::instance().types()) {
        const NodeInfo* inf = NodeRegistry::instance().find(type);
        if (inf->hidden) continue;
        Graph g;
        Node* n = g.addNode(type);
        REQUIRE(n);
        if (n->info().outputs.empty()) continue;
        CAPTURE(type);
        CHECK_NOTHROW(evalOut(g, n->id));
    }
}
