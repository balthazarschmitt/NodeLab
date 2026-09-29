#include <doctest/doctest.h>

#include "graph/Evaluator.h"
#include "nodes/group/GroupNodes.h"

namespace {

Node* constImage(Graph& g, float r, float gr, float b) {
    Node* c = g.addNode("color.combine_rgb");
    c->params[0] = r;
    c->params[1] = gr;
    c->params[2] = b;
    return c;
}

// Evaluates what the Output node shows and returns its first pixel.
std::array<float, 3> outputPixel(Graph& g) {
    EvalContext ctx;
    ctx.defaultW = 4;
    ctx.defaultH = 2;
    Evaluator ev;
    ImagePtr img = ev.evaluateDisplay(g, g.firstOfType("io.output"), ctx);
    REQUIRE(img);
    const float* p = img->pixel(0);
    return {p[0], p[1], p[2]};
}

// src -> invert -> saturation(amount from split R) -> output
struct Chain {
    Graph g;
    int src, split, inv, sat, out;
    Chain() {
        src = constImage(g, 0.8f, 0.3f, 0.1f)->id;
        split = g.addNode("color.split_rgb")->id;
        inv = g.addNode("color.invert")->id;
        sat = g.addNode("color.saturation")->id;
        out = g.addNode("io.output")->id;
        g.connect(src, 0, split, 0);
        g.connect(src, 0, inv, 0);
        g.connect(inv, 0, sat, 0);
        g.connect(split, 0, sat, 1);
        g.connect(sat, 0, out, 0);
    }
};

}  // namespace

TEST_CASE("grouping preserves the result") {
    Chain c;
    auto before = outputPixel(c.g);

    int gid = groupNodes(c.g, {c.inv, c.sat});
    REQUIRE(gid);
    auto* group = dynamic_cast<GroupNode*>(c.g.find(gid));
    REQUIRE(group);
    CHECK(group->ins.size() == 2);   // image from src, R channel from split
    CHECK(group->outs.size() == 1);  // saturation output
    CHECK_FALSE(c.g.find(c.inv));
    auto grouped = outputPixel(c.g);
    for (int k = 0; k < 3; ++k) CHECK(grouped[k] == doctest::Approx(before[k]));

    SUBCASE("survives save/load") {
        Graph g2;
        g2.fromJson(c.g.toJson());
        auto loaded = outputPixel(g2);
        for (int k = 0; k < 3; ++k) CHECK(loaded[k] == doctest::Approx(before[k]));
    }
    SUBCASE("ungroup restores the graph") {
        auto ids = ungroupNode(c.g, gid);
        CHECK(ids.size() == 2);
        CHECK_FALSE(c.g.find(gid));
        auto un = outputPixel(c.g);
        for (int k = 0; k < 3; ++k) CHECK(un[k] == doctest::Approx(before[k]));
    }
    SUBCASE("preview a node inside the group") {
        EvalContext ctx;
        ctx.defaultW = 4;
        ctx.defaultH = 2;
        Evaluator ev;
        int innerInvert = 0;
        for (const auto& [id, n] : group->inner().nodes())
            if (n->info().type == "color.invert") innerInvert = id;
        REQUIRE(innerInvert);
        ImagePtr img = ev.evaluateDisplayPath(c.g, {gid, innerInvert}, ctx);
        REQUIRE(img);
        CHECK(img->pixel(0)[0] == doctest::Approx(1.0f - 0.8f));
    }
    SUBCASE("removing an interface pin drops its wires") {
        group->removePin(c.g, false, 1);
        CHECK(group->ins.size() == 1);
        for (const Link& l : c.g.links())
            if (l.toNode == gid) CHECK(l.toPin == 0);
    }
}

TEST_CASE("groups nest") {
    Chain c;
    auto before = outputPixel(c.g);
    int g1 = groupNodes(c.g, {c.inv, c.sat});
    int g2 = groupNodes(c.g, {c.split, g1});
    REQUIRE(g2);
    auto nested = outputPixel(c.g);
    for (int k = 0; k < 3; ++k) CHECK(nested[k] == doctest::Approx(before[k]));
    CHECK(resolveGroupPath(c.g, {g2, g1}) != nullptr);
    CHECK(resolveGroupPath(c.g, {g1}) == nullptr);  // g1 now lives inside g2
}

TEST_CASE("frames round-trip") {
    Graph g;
    Frame* f = g.addFrame(10, 20, 300, 200);
    f->label = "Sky";
    f->color[0] = 0.5f;
    Graph g2;
    g2.fromJson(g.toJson());
    REQUIRE(g2.frames().size() == 1);
    CHECK(g2.frames()[0].label == "Sky");
    CHECK(g2.frames()[0].w == doctest::Approx(300.0f));
    CHECK(g2.frames()[0].color[0] == doctest::Approx(0.5f));
}
