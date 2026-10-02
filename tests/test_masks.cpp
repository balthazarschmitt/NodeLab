// Local-adjustment masks: Brush Mask's Auto Mask and the Range Mask node.
#include <doctest/doctest.h>

#include <cmath>

#include "graph/Graph.h"
#include "graph/NodeRegistry.h"
#include "graph/Recipes.h"
#include "nodes/matte/MatteNodes.h"

namespace {

// Left half one colour, right half another (sRGB-encoded values, as in a legacy project).
ImagePtr halves(const float a[3], const float b[3], int w = 64, int h = 32) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float* c = x < w / 2 ? a : b;
            float* p = img->pixel(size_t(y) * w + x);
            p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = 1.0f;
        }
    return img;
}

ChannelPtr run(Node* n, std::vector<Value> in, bool linear = false, int w = 64, int h = 32) {
    std::vector<Value> out(n->info().outputs.size());
    EvalContext ctx;
    ctx.defaultW = w, ctx.defaultH = h;
    if (linear) ctx.colorManagement = ColorManagement::sceneLinear();
    in.resize(n->info().inputs.size());
    n->evaluate(ctx, in, out);
    ChannelPtr c = toChannel(out[0]);
    REQUIRE(c);
    return c;
}

float at(const Channel& c, int x, int y) { return c.data[size_t(y) * c.w + x]; }

void set(Node* n, const char* name, const nlohmann::json& v) {
    const auto& ps = n->info().params;
    for (size_t i = 0; i < ps.size(); ++i)
        if (ps[i].name == name) {
            n->params[i] = v;
            return;
        }
    FAIL("no param " << name);
}

const float kRed[3] = {0.8f, 0.15f, 0.1f}, kBlue[3] = {0.1f, 0.2f, 0.8f};

}  // namespace

TEST_CASE("Brush Auto Mask stops at a colour edge") {
    const int w = 64, h = 32;
    const ImagePtr img = halves(kRed, kBlue, w, h);
    BrushMaskNode::Stroke st;
    st.radius = 0.3f;  // 19 px: reaches well into the blue half
    st.feather = 0.0f;
    st.pts = {{0.3f, 0.5f}, {0.4f, 0.5f}};  // painted on the red side

    std::vector<float> plain(size_t(w) * h, 0.0f), masked = plain;
    paintStrokes(plain, w, h, {st});
    st.autoMask = true;
    paintStrokes(masked, w, h, {st}, img.get(), true);
    // Without Auto Mask the brush spills over the edge; with it the blue side stays clear.
    CHECK(plain[size_t(16) * w + 40] == doctest::Approx(1.0f));
    CHECK(masked[size_t(16) * w + 40] == doctest::Approx(0.0f));
    CHECK(masked[size_t(16) * w + 28] == doctest::Approx(1.0f));

    // Without an image, Auto Mask strokes paint as usual.
    std::vector<float> noImage(size_t(w) * h, 0.0f);
    paintStrokes(noImage, w, h, {st});
    CHECK(noImage == plain);
}

TEST_CASE("Brush Mask keeps Auto Mask per stroke, through save and load") {
    Graph g;
    auto* n = dynamic_cast<BrushMaskNode*>(g.addNode("matte.brush_mask"));
    REQUIRE(n);
    n->beginStroke(0.5f, 0.5f, false);
    set(n, "Auto Mask", true);
    n->beginStroke(0.2f, 0.2f, false);
    REQUIRE(n->strokes.size() == 2);
    CHECK_FALSE(n->strokes[0].autoMask);
    CHECK(n->strokes[1].autoMask);
    const std::string sig = n->signatureExtra();

    nlohmann::json j;
    n->saveExtra(j);
    CHECK_FALSE(j["strokes"][0].contains("auto"));  // older files stay as they were
    BrushMaskNode copy;
    copy.loadExtra(j);
    CHECK(copy.strokes[1].autoMask);
    n->strokes[1].autoMask = false;
    CHECK(n->signatureExtra() != sig);  // the cache sees the change
}

TEST_CASE("Range Mask Luminance selects a band of lightness and intersects its Mask") {
    Graph g;
    Node* n = g.addNode("matte.range_mask");
    REQUIRE(n);
    // A grey ramp, black to white, in linear light.
    const int w = 101, h = 2;
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img->pixel(size_t(y) * w + x);
            p[0] = p[1] = p[2] = std::pow(x / 100.0f, 3.0f), p[3] = 1.0f;  // Oklab L = x / 100
        }
    set(n, "Low", 0.4f);
    set(n, "High", 0.6f);
    set(n, "Smoothness", 0.0f);
    ChannelPtr m = run(n, {Value(img)}, true, w, h);
    CHECK(at(*m, 20, 0) == doctest::Approx(0.0f));
    CHECK(at(*m, 50, 0) == doctest::Approx(1.0f).epsilon(1e-3));
    CHECK(at(*m, 80, 0) == doctest::Approx(0.0f));

    // Smoothness feathers the ends instead of cutting.
    set(n, "Smoothness", 1.0f);
    m = run(n, {Value(img)}, true, w, h);
    CHECK(at(*m, 30, 0) > 0.05f);
    CHECK(at(*m, 30, 0) < 0.95f);

    // A Mask is intersected: half strength in, half strength out.
    auto half = std::make_shared<Channel>(Channel::makeConstant(0.5f));
    set(n, "Smoothness", 0.0f);
    m = run(n, {Value(img), Value(ChannelPtr(half))}, true, w, h);
    CHECK(at(*m, 50, 0) == doctest::Approx(0.5f).epsilon(1e-3));
    CHECK(at(*m, 20, 0) == doctest::Approx(0.0f));

    set(n, "Invert", true);
    m = run(n, {Value(img)}, true, w, h);
    CHECK(at(*m, 20, 0) == doctest::Approx(1.0f));
    CHECK(at(*m, 50, 0) == doctest::Approx(0.0f).epsilon(1e-3));
}

TEST_CASE("Range Mask Color selects the picked colour") {
    Graph g;
    Node* n = g.addNode("matte.range_mask");
    REQUIRE(n);
    set(n, "Mode", 1);
    // Legacy project: the param and the pixels are both sRGB-encoded.
    set(n, "Color", nlohmann::json::array({kRed[0], kRed[1], kRed[2]}));
    const ImagePtr img = halves(kRed, kBlue);
    ChannelPtr m = run(n, {Value(img)});
    CHECK(at(*m, 10, 5) == doctest::Approx(1.0f));
    CHECK(at(*m, 50, 5) == doctest::Approx(0.0f));
    // A slightly different red is still taken in at the default Amount, and not at 0.
    const float nearRed[3] = {0.75f, 0.18f, 0.12f};
    const ImagePtr img2 = halves(nearRed, kBlue);
    CHECK(at(*run(n, {Value(img2)}), 10, 5) > 0.9f);
    set(n, "Amount", 0.0f);
    CHECK(at(*run(n, {Value(img2)}), 10, 5) < 0.5f);
}

TEST_CASE("Add Mask inserts a Basic before the Output, driven by a new mask") {
    Graph g;
    Node* in = g.addNode("io.image_input");
    Node* dn = g.addNode("filter.denoise", 200, 0);
    Node* out = g.addNode("io.output", 400, 0);
    g.connect(in->id, 0, dn->id, 0);
    g.connect(dn->id, 0, out->id, 0);

    const recipes::AddedMask a = recipes::addMask(g, recipes::MaskKind::Brush);
    REQUIRE(a.ok());
    CHECK(g.find(a.adjust)->info().type == "color.basic");
    CHECK(g.find(a.adjust)->label == "Mask 1");
    CHECK(g.find(a.mask)->info().type == "matte.brush_mask");
    // Denoise -> Basic -> Output, mask -> Factor, and the image under it -> the brush's Image.
    CHECK(g.inputLink(a.adjust, 0)->fromNode == dn->id);
    CHECK(g.inputLink(out->id, 0)->fromNode == a.adjust);
    CHECK(g.inputLink(a.adjust, 1)->fromNode == a.mask);
    CHECK(g.inputLink(a.mask, 1)->fromNode == dn->id);
    CHECK(g.inputLink(a.mask, 0) == nullptr);

    // A second mask stacks after the first.
    const recipes::AddedMask b = recipes::addMask(g, recipes::MaskKind::Range);
    REQUIRE(b.ok());
    CHECK(g.find(b.adjust)->label == "Mask 2");
    CHECK(g.inputLink(b.adjust, 0)->fromNode == a.adjust);
    CHECK(g.inputLink(out->id, 0)->fromNode == b.adjust);
    CHECK(g.inputLink(b.mask, 0)->fromNode == a.adjust);

    // Gradients take no image.
    const recipes::AddedMask c = recipes::addMask(g, recipes::MaskKind::Linear);
    REQUIRE(c.ok());
    CHECK(g.inputLink(c.mask, 0) == nullptr);

    // Nothing to adjust: no change.
    Graph empty;
    empty.addNode("io.output");
    CHECK_FALSE(recipes::addMask(empty, recipes::MaskKind::Radial).ok());
    CHECK(empty.nodes().size() == 1);
}

TEST_CASE("Mask Selected Nodes blends the chain's edit through a Mix") {
    Graph g;
    Node* in = g.addNode("io.image_input");
    Node* a = g.addNode("color.basic", 200, 0);
    Node* b = g.addNode("filter.denoise", 400, 0);
    Node* out = g.addNode("io.output", 600, 0);
    Node* view = g.addNode("util.file_output", 600, 200);
    g.connect(in->id, 0, a->id, 0);
    g.connect(a->id, 0, b->id, 0);
    g.connect(b->id, 0, out->id, 0);
    g.connect(b->id, 0, view->id, 0);

    const recipes::AddedMask m = recipes::maskNodes(g, {a->id, b->id}, recipes::MaskKind::Range);
    REQUIRE(m.ok());
    CHECK(g.find(m.adjust)->info().type == "math.mix");
    CHECK(g.find(m.adjust)->label == "Mask 1");
    // A = what entered the chain, B = its edit, Factor = the mask; both of the chain's outgoing
    // wires now leave the Mix, and the Range looks at the unedited image.
    CHECK(g.inputLink(m.adjust, 0)->fromNode == in->id);
    CHECK(g.inputLink(m.adjust, 1)->fromNode == b->id);
    CHECK(g.inputLink(m.adjust, 2)->fromNode == m.mask);
    CHECK(g.inputLink(out->id, 0)->fromNode == m.adjust);
    CHECK(g.inputLink(view->id, 0)->fromNode == m.adjust);
    CHECK(g.inputLink(m.mask, 0)->fromNode == in->id);
    CHECK(g.inputLink(a->id, 0)->fromNode == in->id);  // the chain itself is untouched
    CHECK(out->x == doctest::Approx(830.0f));          // downstream moved right
    CHECK(in->x == doctest::Approx(0.0f));

    // A chain at the end of the graph still gets its Mix; gradients take no image.
    Graph g2;
    Node* i2 = g2.addNode("io.image_input");
    Node* c2 = g2.addNode("color.basic", 200, 0);
    g2.connect(i2->id, 0, c2->id, 0);
    const recipes::AddedMask m2 = recipes::maskNodes(g2, {c2->id}, recipes::MaskKind::Linear);
    REQUIRE(m2.ok());
    CHECK(g2.inputLink(m2.adjust, 1)->fromNode == c2->id);
    CHECK(g2.inputLink(m2.mask, 0) == nullptr);

    // Nothing with an image output: no change.
    Graph g3;
    Node* v3 = g3.addNode("io.number");
    CHECK_FALSE(recipes::maskNodes(g3, {v3 ? v3->id : 0}, recipes::MaskKind::Radial).ok());
    CHECK(g3.nodes().size() == (v3 ? 1u : 0u));
}

TEST_CASE("Brush Mask's paint cache matches painting every stroke") {
    const int w = 96, h = 64;
    const ImagePtr img = halves(kRed, kBlue, w, h);
    Graph g;
    auto& n = *dynamic_cast<BrushMaskNode*>(g.addNode("matte.brush_mask"));
    auto evalNow = [&]() {
        std::vector<Value> out(1);
        EvalContext ctx;
        ctx.defaultW = w, ctx.defaultH = h;
        n.evaluate(ctx, {Value(), Value(img)}, out);
        return toChannel(out[0]);
    };
    auto fresh = [&]() {
        std::vector<float> m(size_t(w) * h, 0.0f);
        paintStrokes(m, w, h, n.strokes, img.get(), true);
        return m;
    };
    // Dabs extend the last stroke, new strokes start, one with Auto Mask, one erasing: after
    // each change the (cached) evaluation must equal painting everything from scratch.
    for (int s = 0; s < 4; ++s) {
        BrushMaskNode::Stroke st;
        st.radius = 0.08f + 0.02f * s;
        st.flow = s == 1 ? 0.5f : 1.0f;
        st.erase = s == 3;
        st.autoMask = s == 2;
        n.strokes.push_back(st);
        for (int d = 0; d < 5; ++d) {
            n.strokes.back().pts.push_back({0.1f + 0.15f * d, 0.2f + 0.15f * s});
            CHECK(evalNow()->data == fresh());
        }
    }
    // Undoing a stroke (a shorter list) also matches.
    n.strokes.pop_back();
    CHECK(evalNow()->data == fresh());
}

TEST_CASE("a mask at half strength gives half the stops in linear projects, as in Lightroom") {
    // Grey 0.18 in linear light; Exposure -2 EV through a Factor of 0.5 should land near -1 EV,
    // where mixing linear light gave -0.68 EV.
    const float grey[3] = {0.18f, 0.18f, 0.18f};
    auto img = halves(grey, grey, 8, 4);
    auto expose = [&](float ev, float factor) {
        Graph g;
        Node* b = g.addNode("color.basic");
        set(b, "Exposure", ev);
        std::vector<Value> in{Value(ImagePtr(img)), Value(ChannelPtr(std::make_shared<Channel>(Channel::makeConstant(factor))))};
        std::vector<Value> out(1);
        EvalContext ctx;
        ctx.defaultW = 8, ctx.defaultH = 4;
        ctx.colorManagement = ColorManagement::sceneLinear();
        b->evaluate(ctx, in, out);
        return toImage(out[0], 8, 4)->pixel(0)[1];
    };
    const float full = expose(-1.0f, 1.0f), half = expose(-2.0f, 0.5f);
    CHECK(std::log2(half / 0.18f) == doctest::Approx(std::log2(full / 0.18f)).epsilon(0.05));
}

TEST_CASE("Linear Gradient fades evenly from Start to End in linear projects") {
    Graph g;
    Node* n = g.addNode("matte.linear_gradient");
    set(n, "Start X", 0.0f), set(n, "Start Y", 0.0f), set(n, "End X", 1.0f), set(n, "End Y", 0.0f);
    ChannelPtr lin = run(n, {}, true, 100, 2);
    CHECK(at(*lin, 24, 0) == doctest::Approx(0.75f).epsilon(0.01));
    CHECK(at(*lin, 49, 0) == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(at(*lin, 74, 0) == doctest::Approx(0.25f).epsilon(0.01));
    // Legacy projects keep their smoothstep, so they render as before.
    ChannelPtr legacy = run(n, {}, false, 100, 2);
    CHECK(at(*legacy, 24, 0) > 0.8f);
}
