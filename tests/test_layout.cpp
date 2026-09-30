#include <doctest/doctest.h>

#include "graph/Layout.h"
#include "graph/NodeRegistry.h"

namespace {

NodeSize fixedSize(const Node&) { return {200, 100}; }

bool overlaps(const Node& a, const Node& b) {
    return a.x < b.x + 200 && b.x < a.x + 200 && a.y < b.y + 100 && b.y < a.y + 100;
}

}  // namespace

TEST_CASE("swapNode keeps id, position, links and shared params") {
    Graph g;
    Node* src = g.addNode("color.combine_rgb");
    Node* exp = g.addNode("color.exposure", 300, 40);
    Node* out = g.addNode("io.output", 600, 0);
    const int id = exp->id;
    exp->muted = true;
    g.connect(src->id, 0, id, 0);
    g.connect(id, 0, out->id, 0);

    Node* s = g.swapNode(id, "color.gamma");
    REQUIRE(s);
    CHECK(s->id == id);
    CHECK(s->info().type == "color.gamma");
    CHECK(s->x == 300);
    CHECK(s->y == 40);
    CHECK(s->muted);
    CHECK(g.find(id) == s);
    // Both the image input and the output are still wired.
    REQUIRE(g.inputLink(id, 0));
    CHECK(g.inputLink(id, 0)->fromNode == src->id);
    REQUIRE(g.inputLink(out->id, 0));
    CHECK(g.inputLink(out->id, 0)->fromNode == id);

    CHECK(g.swapNode(id, "no.such.type") == nullptr);
    CHECK(g.find(id) == s);
}

TEST_CASE("spaceOut pushes overlapping nodes clear of the anchor") {
    Graph g;
    Node* a = g.addNode("color.invert", 0, 0);
    Node* b = g.addNode("color.invert", 50, 10);    // mostly overlapping, to the right
    Node* c = g.addNode("color.invert", 1000, 0);   // far away: untouched
    Node* d = g.addNode("color.invert", 60, 500);   // downstream of b, carried along
    g.connect(b->id, 0, d->id, 0);
    const float dx0 = d->x;
    CHECK(spaceOut(g, {a->id}, fixedSize));
    CHECK(a->x == 0);
    CHECK(a->y == 0);
    CHECK_FALSE(overlaps(*a, *b));
    CHECK(c->x == 1000);
    if (b->x != 50) CHECK(d->x - dx0 == b->x - 50);  // sideways push moved the chain too
    CHECK_FALSE(spaceOut(g, {a->id}, fixedSize));    // nothing left to do
}

TEST_CASE("arrangeNodes lays a chain out left to right without overlaps") {
    Graph g;
    Node* in = g.addNode("color.combine_rgb", 500, 500);
    Node* inv = g.addNode("color.invert", 510, 505);
    Node* gam = g.addNode("color.gamma", 480, 520);
    Node* mix = g.addNode("color.invert", 490, 510);
    Node* out = g.addNode("io.output", 520, 530);
    g.connect(in->id, 0, inv->id, 0);
    g.connect(in->id, 0, gam->id, 0);
    g.connect(inv->id, 0, mix->id, 0);
    g.connect(mix->id, 0, out->id, 0);
    std::set<int> all;
    for (const auto& [id, n] : g.nodes()) all.insert(id);
    CHECK(arrangeNodes(g, all, fixedSize));
    CHECK(in->x < inv->x);
    CHECK(inv->x < mix->x);
    CHECK(mix->x < out->x);
    CHECK(gam->x == inv->x);  // same depth, same column
    std::vector<Node*> v{in, inv, gam, mix, out};
    for (size_t i = 0; i < v.size(); ++i)
        for (size_t j = i + 1; j < v.size(); ++j) CHECK_FALSE(overlaps(*v[i], *v[j]));
    // Keeps the block's top-left corner.
    CHECK(in->x == 480);
    CHECK(in->y == 500);
}
