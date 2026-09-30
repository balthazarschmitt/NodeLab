// Node changes made while building the infrared foliage recipe: Normalize percentiles, unclamped
// Combine RGB, Blur's Relative size, and params that show only when another param is set.
#include <doctest/doctest.h>

#include <cmath>

#include "graph/Graph.h"
#include "graph/NodeRegistry.h"

namespace {

Value run(Node& n, std::vector<Value> in, float scale = 1.0f) {
    const NodeInfo& info = n.info();
    in.resize(info.inputs.size());
    for (size_t i = 0; i < info.inputs.size(); ++i)
        if (in[i].empty() && info.inputs[i].fallbackParam >= 0) in[i] = Value(n.paramF(info.inputs[i].fallbackParam));
    std::vector<Value> out(info.outputs.size());
    EvalContext ctx;
    ctx.defaultW = 8, ctx.defaultH = 8;
    ctx.scale = scale;
    n.evaluate(ctx, in, out);
    return out[0];
}

ChannelPtr ramp100() {
    // 0, 1, ..., 99 in a 10 x 10 channel
    auto c = std::make_shared<Channel>(Channel::makeSized(10, 10));
    for (int i = 0; i < 100; ++i) c->data[size_t(i)] = float(i);
    return c;
}

ImagePtr impulse(int w, int h) {
    auto img = std::make_shared<Image>(w, h);
    for (size_t i = 0; i < size_t(w) * h; ++i) img->pixel(i)[3] = 1.0f;
    float* p = img->pixel(size_t(h / 2) * w + w / 2);
    p[0] = p[1] = p[2] = 1.0f;
    return img;
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

TEST_CASE_FIXTURE(Fixture, "Normalize defaults to min/max and takes percentiles") {
    Node* n = add("conv.normalize");
    ChannelPtr out = toChannel(run(*n, {Value(ramp100())}));
    REQUIRE(out);
    CHECK(out->data[0] == doctest::Approx(0.0f));
    CHECK(out->data[99] == doctest::Approx(1.0f));

    n->params[0] = 10.0f;  // Low %
    n->params[1] = 90.0f;  // High %
    out = toChannel(run(*n, {Value(ramp100())}));
    // 10th percentile of 0..99 is ~10, the 90th ~89; values outside are not clamped
    CHECK(out->data[10] == doctest::Approx(0.0f).epsilon(0.02));
    CHECK(out->data[89] == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(out->data[0] < -0.1f);
    CHECK(out->data[99] > 1.1f);
}

TEST_CASE_FIXTURE(Fixture, "Combine RGB passes values outside 0..1 like Blender") {
    Node* n = add("color.combine_rgb");
    auto big = std::make_shared<Channel>(Channel::makeConstant(2.5f));
    auto neg = std::make_shared<Channel>(Channel::makeConstant(-0.5f));
    ImagePtr img = toImage(run(*n, {Value(ChannelPtr(big)), Value(ChannelPtr(neg)), Value(0.25f)}), 4, 4);
    REQUIRE(img);
    CHECK(img->pixel(0)[0] == doctest::Approx(2.5f));
    CHECK(img->pixel(0)[1] == doctest::Approx(-0.5f));
    CHECK(img->pixel(0)[2] == doctest::Approx(0.25f));
}

TEST_CASE_FIXTURE(Fixture, "Blur Relative sizes follow the image resolution") {
    Node* rel = add("filter.blur");
    rel->params[2] = true;  // Relative
    rel->params[3] = 1;     // Aspect Correction Y: both axes measured against the width
    rel->params[4] = 10.0f;
    rel->params[5] = 10.0f;
    Node* abs = add("filter.blur");

    for (int w : {40, 80}) {
        abs->params[0] = abs->params[1] = 0.1f * w;  // the same size in pixels
        ImagePtr a = toImage(run(*rel, {Value(impulse(w, w / 2))}), 0, 0);
        // Relative ignores ctx.scale: the working image is already proxy-sized
        ImagePtr r = toImage(run(*rel, {Value(impulse(w, w / 2))}, 0.25f), 0, 0);
        ImagePtr b = toImage(run(*abs, {Value(impulse(w, w / 2))}), 0, 0);
        REQUIRE(a);
        REQUIRE(b);
        float m = 0, mr = 0;
        for (size_t i = 0; i < a->px.size(); ++i) {
            m = std::max(m, std::fabs(a->px[i] - b->px[i]));
            mr = std::max(mr, std::fabs(a->px[i] - r->px[i]));
        }
        CHECK(m < 1e-5f);
        CHECK(mr < 1e-5f);
    }
}

TEST_CASE_FIXTURE(Fixture, "Blur shows Factor X/Y only when Relative is on") {
    Node* n = add("filter.blur");
    CHECK(n->paramVisible(0));   // Size X
    CHECK(!n->paramVisible(3));  // Aspect Correction
    CHECK(!n->paramVisible(4));  // Factor X
    n->params[2] = true;
    CHECK(!n->paramVisible(0));
    CHECK(n->paramVisible(3));
    CHECK(n->paramVisible(4));
    CHECK(n->paramVisible(2));  // Relative itself is always shown
}
