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

TEST_CASE("bridge (delete with reconnect) and mute") {
    Chain c;  // src -> invert -> saturation -> output, split R -> saturation.Amount
    auto before = outputPixel(c.g);

    SUBCASE("muting a node passes its image input through") {
        c.g.find(c.inv)->muted = true;
        auto muted = outputPixel(c.g);
        c.g.find(c.inv)->muted = false;
        c.g.bridgeNode(c.inv);  // equivalent graph: src straight into saturation
        c.g.removeNode(c.inv);
        auto bridged = outputPixel(c.g);
        for (int k = 0; k < 3; ++k) CHECK(muted[k] == doctest::Approx(bridged[k]));
        CHECK(muted[0] != doctest::Approx(before[0]));
    }
    SUBCASE("deleting consecutive nodes keeps the chain connected") {
        c.g.bridgeNode(c.inv);
        c.g.removeNode(c.inv);
        c.g.bridgeNode(c.sat);
        c.g.removeNode(c.sat);
        const Link* l = c.g.inputLink(c.out, 0);
        REQUIRE(l);
        CHECK(l->fromNode == c.src);
    }
    SUBCASE("flags survive save/load") {
        Node* n = c.g.find(c.inv);
        n->muted = true;
        n->collapsed = true;
        n->label = "Negative";
        Graph g2;
        g2.fromJson(c.g.toJson());
        Node* m = g2.find(c.inv);
        CHECK(m->muted);
        CHECK(m->collapsed);
        CHECK(m->label == "Negative");
    }
}

TEST_CASE("group inputs have values, like Blender's group sockets") {
    Chain c;
    const Node* satNode = c.g.find(c.sat);
    const PinDesc& amountPin = satNode->info().inputs[1];
    REQUIRE(amountPin.fallbackParam >= 0);
    // Copies: grouping moves the node into the group.
    const ParamDesc amount = satNode->info().params[size_t(amountPin.fallbackParam)];
    const int amountParam = amountPin.fallbackParam;
    const std::string satType = satNode->info().type;

    int gid = groupNodes(c.g, {c.inv, c.sat});
    auto* group = dynamic_cast<GroupNode*>(c.g.find(gid));
    REQUIRE(group);
    REQUIRE(group->ins.size() == 2);
    // The channel input took the range of the slider it feeds; the image input has no slider.
    CHECK(group->ranges[1].min == amount.min);
    CHECK(group->ranges[1].max == amount.max);
    CHECK(group->info().inputs[1].fallbackParam == 1);
    CHECK(group->info().inputs[0].fallbackParam == -1);
    CHECK_FALSE(group->paramVisible(0));
    CHECK(group->paramVisible(1));

    // Unconnected, the group's value drives the inner node as its own slider would.
    for (const Link& l : c.g.links())
        if (l.toNode == gid && l.toPin == 1) {
            c.g.removeLink(l.id);
            break;
        }
    group->params[1] = 0.25f;
    const auto grouped = outputPixel(c.g);

    Chain ref;
    for (const Link& l : ref.g.links())
        if (l.toNode == ref.sat && l.toPin == 1) {
            ref.g.removeLink(l.id);
            break;
        }
    ref.g.find(ref.sat)->params[size_t(amountParam)] = 0.25f;
    const auto expected = outputPixel(ref.g);
    for (int k = 0; k < 3; ++k) CHECK(grouped[k] == doctest::Approx(expected[k]));

    SUBCASE("values and ranges survive save/load") {
        group->setRange(1, {0.5f, -1.0f, 2.0f});
        Graph g2;
        g2.fromJson(c.g.toJson());
        auto* loaded = dynamic_cast<GroupNode*>(g2.find(gid));
        REQUIRE(loaded);
        CHECK(loaded->paramF(1) == doctest::Approx(0.25f));
        CHECK(loaded->ranges[1].def == doctest::Approx(0.5f));
        CHECK(loaded->ranges[1].min == doctest::Approx(-1.0f));
        CHECK(loaded->ranges[1].max == doctest::Approx(2.0f));
        const auto again = outputPixel(g2);
        for (int k = 0; k < 3; ++k) CHECK(again[k] == doctest::Approx(expected[k]));
    }
    SUBCASE("a range clamps the value") {
        group->setRange(1, {0.0f, 0.5f, 0.2f});  // min > max: max follows min
        CHECK(group->ranges[1].max == doctest::Approx(0.5f));
        CHECK(group->paramF(1) == doctest::Approx(0.5f));
    }
    SUBCASE("values move with their pins") {
        group->movePin(c.g, false, 1, -1);
        CHECK(group->ins[0].type == PinType::Channel);
        CHECK(group->paramF(0) == doctest::Approx(0.25f));
        CHECK(group->info().inputs[0].fallbackParam == 0);
        group->removePin(c.g, false, 1);  // the image input
        REQUIRE(group->params.size() == 1);
        CHECK(group->paramF(0) == doctest::Approx(0.25f));
    }
    SUBCASE("older files take the value of the inner slider") {
        nlohmann::json j = c.g.toJson();
        for (auto& n : j["nodes"])
            if (n["id"] == gid)
                for (auto& in : n["extra"]["inputs"]) in = {{"name", in["name"]}, {"type", in["type"]}};
        Graph g2;
        g2.fromJson(j);
        auto* loaded = dynamic_cast<GroupNode*>(g2.find(gid));
        REQUIRE(loaded);
        CHECK(loaded->params.size() == 2);
        // Not the group's 0.25, which the old file didn't have: the inner node's own slider.
        float inner = -1.0f;
        for (const auto& [id, n] : loaded->inner().nodes())
            if (n->info().type == satType) inner = n->paramF(amountParam);
        CHECK(inner != 0.25f);
        CHECK(loaded->paramF(1) == inner);
        CHECK(loaded->info().inputs[1].fallbackParam == 1);
        // A value saved as null (inner sliders that disagreed) stays empty.
        for (auto& n : j["nodes"])
            if (n["id"] == gid) n["extra"]["inputs"][1]["value"] = nullptr;
        Graph g3;
        g3.fromJson(j);
        auto* none = dynamic_cast<GroupNode*>(g3.find(gid));
        REQUIRE(none);
        CHECK(none->info().inputs[1].fallbackParam == -1);
        CHECK(none->paramHidden(1));
        none->setRange(1, {0.5f, 0.0f, 1.0f});
        CHECK(none->info().inputs[1].fallbackParam == 1);
    }
}

TEST_CASE("Value Input / Output nodes add, rename and remove group sockets") {
    Chain c;
    const int gid = groupNodes(c.g, {c.inv, c.sat});
    auto* group = dynamic_cast<GroupNode*>(c.g.find(gid));
    REQUIRE(group);
    Graph& in = group->inner();
    int innerSat = 0, innerInv = 0;
    for (const auto& [id, n] : in.nodes()) {
        if (n->info().type == "color.saturation") innerSat = id;
        if (n->info().type == "color.invert") innerInv = id;
    }
    REQUIRE(innerSat);

    // Adding a Value Input gives the group a Number input of its own.
    auto* vi = dynamic_cast<GroupValueInputNode*>(in.addNode("group.value_input"));
    REQUIRE(vi);
    CHECK(group->syncValueNodes(c.g));
    REQUIRE(group->ins.size() == 3);
    CHECK(group->ins[2].name == "Value");
    CHECK(group->ins[2].type == PinType::Number);
    CHECK(vi->pin == 2);
    CHECK_FALSE(group->syncValueNodes(c.g));  // nothing more to do

    // It feeds Saturation: a value of 0 makes the result grey.
    in.connect(vi->id, 0, innerSat, 1);
    group->params[2] = 0.0f;
    auto grey = outputPixel(c.g);
    CHECK(grey[0] == doctest::Approx(grey[1]));
    CHECK(grey[1] == doctest::Approx(grey[2]));

    // Renaming the node (F2 sets its label) renames the socket; the title follows the socket.
    vi->label = "Amount";
    CHECK(group->syncValueNodes(c.g));
    CHECK(group->ins[2].name == "Amount");
    CHECK(vi->label.empty());
    CHECK(vi->info().displayName == "Amount");
    CHECK(vi->info().outputs[0].name == "Amount");

    // Saved and loaded, it still renders the same.
    Graph copy;
    copy.fromJson(c.g.toJson());
    auto again = outputPixel(copy);
    for (int k = 0; k < 3; ++k) CHECK(again[k] == doctest::Approx(grey[k]));

    // A Value Output adds an output socket, fed by whatever is wired into it.
    auto* vo = dynamic_cast<GroupValueOutputNode*>(in.addNode("group.value_output"));
    CHECK(group->syncValueNodes(c.g));
    REQUIRE(group->outs.size() == 2);
    group->setPinType(c.g, true, 1, PinType::Image);
    REQUIRE(in.connect(innerInv, 0, vo->id, 0));
    c.g.connect(gid, 1, c.out, 0);
    auto inverted = outputPixel(c.g);
    CHECK(inverted[0] == doctest::Approx(1.0f - 0.8f));
    c.g.connect(gid, 0, c.out, 0);

    // Moving sockets keeps the value nodes on theirs.
    group->movePin(c.g, false, 2, -1);
    CHECK(vi->pin == 1);
    CHECK(group->ins[1].name == "Amount");
    auto moved = outputPixel(c.g);
    for (int k = 0; k < 3; ++k) CHECK(moved[k] == doctest::Approx(grey[k]));

    // Ungrouping wires the value node's socket through like the Group Input's.
    {
        Graph u;
        u.fromJson(c.g.toJson());
        ungroupNode(u, gid);
        for (const auto& [id, n] : u.nodes()) CHECK(n->info().type.rfind("group.", 0) != 0);
        // The socket was unconnected outside, so Saturation is left on its own slider: in colour.
        auto flat = outputPixel(u);
        CHECK(flat[0] == doctest::Approx(1.0f - 0.8f).epsilon(0.01));
        CHECK(flat[2] > flat[0]);
    }

    // Deleting the last node of a socket it made removes the socket.
    in.removeNode(vi->id);
    CHECK(group->syncValueNodes(c.g));
    CHECK(group->ins.size() == 2);
    in.removeNode(vo->id);
    CHECK(group->syncValueNodes(c.g));
    CHECK(group->outs.size() == 1);
}
