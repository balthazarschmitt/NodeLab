#include "graph/Recipes.h"

#include <algorithm>
#include <string>
#include <vector>

namespace recipes {

namespace {

constexpr const char* kOutput = "io.output";
constexpr const char* kBasic = "color.basic";
constexpr int kBasicFactorPin = 1;
constexpr const char* kMix = "math.mix";
constexpr int kMixFactorPin = 2;

const char* maskType(MaskKind k) {
    switch (k) {
        case MaskKind::Linear: return "matte.linear_gradient";
        case MaskKind::Radial: return "matte.radial_gradient";
        case MaskKind::Brush: return "matte.brush_mask";
        default: return "matte.range_mask";
    }
}

// "Mask N", one more than the highest N already used, so names stay unique after deletions.
std::string nextLabel(const Graph& g) {
    int n = 0;
    for (const auto& [id, node] : g.nodes())
        if (node->label.rfind("Mask ", 0) == 0) {
            try {
                n = std::max(n, std::stoi(node->label.substr(5)));
            } catch (...) {
            }
        }
    return "Mask " + std::to_string(n + 1);
}

// A spot near (x, y) whose box (a node's typical footprint, in canvas units) holds no other
// node's top-left corner: steps down until clear. Node heights aren't known here, so this is
// only a heuristic against stacking the mask exactly on top of something.
void freeSpot(const Graph& g, int self, float x, float& y) {
    for (int i = 0; i < 40; ++i) {
        bool clear = true;
        for (const auto& [id, n] : g.nodes())
            if (id != self && n->x > x - 200.0f && n->x < x + 200.0f && n->y > y - 180.0f && n->y < y + 260.0f) {
                clear = false;
                break;
            }
        if (clear) return;
        y += 80.0f;
    }
}

}  // namespace

AddedMask addMask(Graph& g, MaskKind kind) {
    const int outId = g.firstOfType(kOutput);
    const Link* feed = outId ? g.inputLink(outId, 0) : nullptr;
    if (!feed) return {};
    const int srcNode = feed->fromNode, srcPin = feed->fromPin;
    Node* out = g.find(outId);

    // The Basic takes the Output's place and the Output moves right; the mask goes below and to
    // the left, where its wire into Factor reads naturally.
    const float x = out->x, y = out->y;
    Node* basic = g.addNode(kBasic, x, y);
    Node* mask = g.addNode(maskType(kind), x - 230.0f, y + 220.0f);
    if (!basic || !mask) {
        if (basic) g.removeNode(basic->id);
        if (mask) g.removeNode(mask->id);
        return {};
    }
    out->x = x + 230.0f;
    freeSpot(g, mask->id, mask->x, mask->y);
    basic->label = nextLabel(g);
    g.connect(srcNode, srcPin, basic->id, 0);
    g.connect(basic->id, 0, outId, 0);
    g.connect(mask->id, 0, basic->id, kBasicFactorPin);
    // Range and Brush (Auto Mask) look at the image being adjusted.
    if (kind == MaskKind::Range) g.connect(srcNode, srcPin, mask->id, 0);
    if (kind == MaskKind::Brush) g.connect(srcNode, srcPin, mask->id, 1);
    return {basic->id, mask->id};
}

AddedMask maskNodes(Graph& g, const std::set<int>& ids, MaskKind kind) {
    std::vector<Node*> chain;
    for (int id : ids)
        if (Node* n = g.find(id)) chain.push_back(n);
    if (chain.empty()) return {};
    std::sort(chain.begin(), chain.end(), [](Node* a, Node* b) { return a->x != b->x ? a->x < b->x : a->y < b->y; });
    auto inside = [&](int id) { return ids.count(id) > 0; };

    // The exit: the rightmost node's image output that leaves the chain (else its first image
    // output, for a chain at the end of the graph).
    int exitNode = 0, exitPin = -1;
    for (auto it = chain.rbegin(); it != chain.rend() && exitPin < 0; ++it) {
        for (const Link& l : g.links())
            if (l.fromNode == (*it)->id && !inside(l.toNode) && (*it)->info().outputs[size_t(l.fromPin)].type == PinType::Image) {
                exitNode = l.fromNode, exitPin = l.fromPin;
                break;
            }
    }
    if (exitPin < 0) {
        const auto& outs = chain.back()->info().outputs;
        for (int o = 0; o < int(outs.size()) && exitPin < 0; ++o)
            if (outs[size_t(o)].type == PinType::Image) exitNode = chain.back()->id, exitPin = o;
    }
    if (exitPin < 0) return {};

    // The entry: the first image wire coming in from outside, leftmost node first.
    int entryNode = 0, entryPin = 0;
    for (Node* n : chain) {
        const auto& ins = n->info().inputs;
        for (int i = 0; i < int(ins.size()) && !entryNode; ++i)
            if (const Link* l = g.inputLink(n->id, i); l && !inside(l->fromNode) && ins[size_t(i)].type == PinType::Image)
                entryNode = l->fromNode, entryPin = l->fromPin;
        if (entryNode) break;
    }

    Node* exit = g.find(exitNode);
    const float x = exit->x + 230.0f, y = exit->y;
    float bottom = y;
    for (Node* n : chain) bottom = std::max(bottom, n->y);

    // Everything downstream of the exit moves right, so the Mix has room.
    std::set<int> down;
    std::vector<int> todo{exitNode};
    while (!todo.empty()) {
        const int id = todo.back();
        todo.pop_back();
        for (const Link& l : g.links())
            if (l.fromNode == id && !inside(l.toNode) && down.insert(l.toNode).second) todo.push_back(l.toNode);
    }
    std::vector<Link> outgoing;
    for (const Link& l : g.links())
        if (l.fromNode == exitNode && l.fromPin == exitPin && !inside(l.toNode)) outgoing.push_back(l);

    Node* mix = g.addNode(kMix, x, y);
    Node* mask = g.addNode(maskType(kind), chain.front()->x, bottom + 260.0f);
    if (!mix || !mask) {
        if (mix) g.removeNode(mix->id);
        if (mask) g.removeNode(mask->id);
        return {};
    }
    for (int id : down)
        if (Node* n = g.find(id)) n->x += 230.0f;
    freeSpot(g, mask->id, mask->x, mask->y);
    mix->label = nextLabel(g);
    for (const Link& l : outgoing) g.connect(mix->id, 0, l.toNode, l.toPin);
    if (entryNode) g.connect(entryNode, entryPin, mix->id, 0);
    g.connect(exitNode, exitPin, mix->id, 1);
    g.connect(mask->id, 0, mix->id, kMixFactorPin);
    if (entryNode && kind == MaskKind::Range) g.connect(entryNode, entryPin, mask->id, 0);
    if (entryNode && kind == MaskKind::Brush) g.connect(entryNode, entryPin, mask->id, 1);
    return {mix->id, mask->id};
}

}  // namespace recipes
