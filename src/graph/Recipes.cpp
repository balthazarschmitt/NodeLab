#include "graph/Recipes.h"

#include <string>

namespace recipes {

namespace {

constexpr const char* kOutput = "io.output";
constexpr const char* kBasic = "color.basic";
constexpr int kBasicFactorPin = 1;

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

}  // namespace recipes
