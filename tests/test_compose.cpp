// Compositing nodes: Layer Stack.
#include <doctest/doctest.h>

#include "graph/Evaluator.h"
#include "graph/NodeRegistry.h"
#include "nodes/math/LayerStack.h"

namespace {

Value evalOut(Graph& g, int id) {
    EvalContext ctx;
    ctx.defaultW = 4;
    ctx.defaultH = 2;
    Evaluator ev;
    return ev.evaluateOutput(g, id, 0, ctx);
}

Node* constImage(Graph& g, float r, float gr, float b) {
    Node* c = g.addNode("color.combine_rgb");
    c->params[0] = r;
    c->params[1] = gr;
    c->params[2] = b;
    return c;
}

const float* px(const Value& v) {
    auto p = std::get_if<ImagePtr>(&v.v);
    REQUIRE(p);
    REQUIRE(*p);
    return (*p)->pixel(0);
}

LayerStackNode* stack(Graph& g) {
    auto* ls = dynamic_cast<LayerStackNode*>(g.addNode(LayerStackNode::kType));
    REQUIRE(ls);
    return ls;
}

int imagePin(int layer) { return layer * LayerStackNode::kStride; }
int opacityParam(int layer) { return layer * LayerStackNode::kStride; }
int modeParam(int layer) { return layer * LayerStackNode::kStride + 1; }

}  // namespace

TEST_CASE("Layer Stack layers upwards from Layer 0, with opacity") {
    Graph g;
    Node* red = constImage(g, 1, 0, 0);
    Node* blue = constImage(g, 0, 0, 1);
    auto* ls = stack(g);
    CHECK(evalOut(g, ls->id).empty());  // nothing connected: nothing out

    g.connect(red->id, 0, ls->id, imagePin(0));
    const float* p = px(evalOut(g, ls->id));
    CHECK(p[0] == doctest::Approx(1.0f));
    CHECK(p[3] == doctest::Approx(1.0f));

    g.connect(blue->id, 0, ls->id, imagePin(1));
    p = px(evalOut(g, ls->id));
    CHECK(p[0] == doctest::Approx(0.0f));  // the top layer covers the bottom
    CHECK(p[2] == doctest::Approx(1.0f));

    ls->params[size_t(opacityParam(1))] = 0.25f;
    p = px(evalOut(g, ls->id));
    CHECK(p[0] == doctest::Approx(0.75f));
    CHECK(p[2] == doctest::Approx(0.25f));

    // A lone layer with opacity is see-through: its alpha drops.
    ls->params[size_t(opacityParam(1))] = 1.0f;
    g.removeLink(g.inputLink(ls->id, imagePin(0))->id);
    ls->params[size_t(opacityParam(1))] = 0.5f;
    p = px(evalOut(g, ls->id));
    CHECK(p[2] == doctest::Approx(1.0f));
    CHECK(p[3] == doctest::Approx(0.5f));
}

TEST_CASE("Layer Stack blends each layer with what's below it") {
    Graph g;
    Node* grey = constImage(g, 0.5f, 0.5f, 0.5f);
    Node* col = constImage(g, 0.5f, 1.0f, 0.0f);
    auto* ls = stack(g);
    g.connect(grey->id, 0, ls->id, imagePin(0));
    g.connect(col->id, 0, ls->id, imagePin(1));
    ls->params[size_t(modeParam(1))] = 2;  // Multiply, as Blend's
    const float* p = px(evalOut(g, ls->id));
    CHECK(p[0] == doctest::Approx(0.25f));
    CHECK(p[1] == doctest::Approx(0.5f));
    CHECK(p[2] == doctest::Approx(0.0f));
    ls->params[size_t(opacityParam(1))] = 0.5f;
    p = px(evalOut(g, ls->id));
    CHECK(p[0] == doctest::Approx(0.375f));
    // The bottom layer's mode changes nothing.
    ls->params[size_t(modeParam(0))] = 5;
    CHECK(px(evalOut(g, ls->id))[0] == doctest::Approx(0.375f));
}

TEST_CASE("Layer Stack grows a layer when the top one is connected") {
    Graph g;
    Node* a = constImage(g, 1, 0, 0);
    auto* ls = stack(g);
    CHECK(ls->layers() == LayerStackNode::kMinLayers);
    g.connect(a->id, 0, ls->id, imagePin(0));
    CHECK(ls->layers() == 2);
    g.connect(a->id, 0, ls->id, imagePin(1));
    CHECK(ls->layers() == 3);
    CHECK(ls->info().inputs.size() == 6);
    // A mask on the top layer's opacity counts as using it.
    g.connect(a->id, 0, ls->id, imagePin(2) + 1);
    CHECK(ls->layers() == 4);
    CHECK(ls->params.size() == ls->info().params.size());
}

TEST_CASE("Layer Stack keeps its layers through save, load, duplicate and edits") {
    Graph g;
    Node* red = constImage(g, 1, 0, 0);
    Node* green = constImage(g, 0, 1, 0);
    Node* blue = constImage(g, 0, 0, 1);
    auto* ls = stack(g);
    g.connect(red->id, 0, ls->id, imagePin(0));
    g.connect(green->id, 0, ls->id, imagePin(1));
    g.connect(blue->id, 0, ls->id, imagePin(2));
    REQUIRE(ls->layers() == 4);
    ls->params[size_t(opacityParam(2))] = 0.5f;
    ls->params[size_t(modeParam(2))] = 5;

    Graph h;
    h.fromJson(g.toJson());
    auto* back = dynamic_cast<LayerStackNode*>(h.find(ls->id));
    REQUIRE(back);
    CHECK(back->layers() == 4);
    CHECK(back->paramF(opacityParam(2)) == doctest::Approx(0.5f));
    CHECK(back->paramI(modeParam(2)) == 5);
    REQUIRE(h.inputLink(ls->id, imagePin(2)));
    CHECK(h.inputLink(ls->id, imagePin(2))->fromNode == blue->id);

    auto* copy = dynamic_cast<LayerStackNode*>(g.cloneNode(*ls, 0, 0));
    REQUIRE(copy);
    CHECK(copy->layers() == 4);
    CHECK(copy->paramI(modeParam(2)) == 5);

    // Moving the blue layer down takes its wire and settings with it.
    ls->moveLayer(g, 2, -1);
    CHECK(g.inputLink(ls->id, imagePin(1))->fromNode == blue->id);
    CHECK(g.inputLink(ls->id, imagePin(2))->fromNode == green->id);
    CHECK(ls->paramI(modeParam(1)) == 5);
    CHECK(px(evalOut(g, ls->id))[1] == doctest::Approx(1.0f));  // green on top

    // Removing the green layer moves the empty one down.
    ls->removeLayer(g, 2);
    CHECK(ls->layers() == 3);
    CHECK(!g.inputLink(ls->id, imagePin(2)));
    CHECK(g.inputLink(ls->id, imagePin(1))->fromNode == blue->id);
    CHECK(g.pruneInvalidLinks() == 0);

    // A damaged layer list loads what it can.
    nlohmann::json j = g.toJson();
    for (auto& n : j["nodes"])
        if (n["type"] == LayerStackNode::kType) n["extra"]["layers"][0] = {{"opacity", "x"}, {"mode", 999}};
    Graph d;
    d.fromJson(j);
    auto* dl = dynamic_cast<LayerStackNode*>(d.find(ls->id));
    REQUIRE(dl);
    CHECK(dl->paramF(opacityParam(0)) == doctest::Approx(1.0f));
    CHECK(dl->paramI(modeParam(0)) < 19);
}
