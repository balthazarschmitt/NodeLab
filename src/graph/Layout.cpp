#include "graph/Layout.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace {

struct Box {
    float x0, y0, x1, y1;
};

Box boxOf(const Node& n, const NodeSizeFn& size) {
    const NodeSize s = size(n);
    return {n.x, n.y, n.x + s.w, n.y + s.h};
}

// Everything reachable from `start` following links downstream (or upstream), start included.
std::set<int> chain(const Graph& g, int start, bool downstream) {
    std::set<int> seen{start};
    std::vector<int> stack{start};
    while (!stack.empty()) {
        const int id = stack.back();
        stack.pop_back();
        for (const Link& l : g.links()) {
            const int from = downstream ? l.fromNode : l.toNode;
            const int to = downstream ? l.toNode : l.fromNode;
            if (from == id && seen.insert(to).second) stack.push_back(to);
        }
    }
    return seen;
}

}  // namespace

bool spaceOut(Graph& g, const std::set<int>& anchors, const NodeSizeFn& size) {
    // Nodes already settled this pass, and so treated as fixed obstacles for later pushes.
    std::set<int> fixed = anchors;
    std::vector<int> queue(anchors.begin(), anchors.end());
    bool moved = false;
    // Each push can cascade into further overlaps; cap the work so a pathological graph can't hang.
    for (size_t qi = 0; qi < queue.size() && qi < 400; ++qi) {
        const Node* a = g.find(queue[qi]);
        if (!a) continue;
        const Box A = boxOf(*a, size);
        for (const auto& [id, np] : g.nodes()) {
            if (fixed.count(id)) continue;
            Node& n = *np;
            const Box B = boxOf(n, size);
            // Only actual overlaps count as crowding; once pushed, a node clears by the full gap.
            if (!(A.x0 < B.x1 && B.x0 < A.x1 && A.y0 < B.y1 && B.y0 < A.y1)) continue;
            const float right = A.x1 + kLayoutGapY - B.x0;  // distance to push B right
            const float left = B.x1 + kLayoutGapY - A.x0;   // ... left
            const float down = A.y1 + kLayoutGapY - B.y0;
            const float up = B.y1 + kLayoutGapY - A.y0;
            const bool toRight = (B.x0 + B.x1) >= (A.x0 + A.x1);
            const bool toBottom = (B.y0 + B.y1) >= (A.y0 + A.y1);
            const float dx = toRight ? right : left;
            const float dy = toBottom ? down : up;
            if (dx <= dy) {
                // Sideways: take the chain along so the wires still flow left to right. Never drag
                // an anchor, which would undo the placement we are making room for.
                const float shift = std::round(toRight ? dx : -dx);
                for (int cid : chain(g, id, toRight))
                    if (!anchors.count(cid))
                        if (Node* c = g.find(cid)) c->x += shift;
            } else {
                n.y += std::round(toBottom ? dy : -dy);
            }
            moved = true;
            fixed.insert(id);
            queue.push_back(id);
        }
    }
    return moved;
}

bool arrangeNodes(Graph& g, const std::set<int>& ids, const NodeSizeFn& size) {
    std::vector<Node*> nodes;
    float minX = 1e30f, minY = 1e30f;
    for (int id : ids)
        if (Node* n = g.find(id)) {
            nodes.push_back(n);
            minX = std::min(minX, n->x);
            minY = std::min(minY, n->y);
        }
    if (nodes.size() < 2) return false;

    // Column = longest path from a source within the set. Links only point downstream and the
    // graph is acyclic, so relaxing |nodes| times converges.
    std::map<int, int> col;
    for (Node* n : nodes) col[n->id] = 0;
    for (size_t pass = 0; pass < nodes.size(); ++pass) {
        bool changed = false;
        for (const Link& l : g.links())
            if (col.count(l.fromNode) && col.count(l.toNode) && col[l.toNode] < col[l.fromNode] + 1) {
                col[l.toNode] = col[l.fromNode] + 1;
                changed = true;
            }
        if (!changed) break;
    }
    // Pull sources right next to their first consumer, so a lone input doesn't sit columns away
    // from where it is used (Blender's arrange does the same).
    for (Node* n : nodes) {
        int nearest = -1;
        for (const Link& l : g.links())
            if (l.fromNode == n->id && col.count(l.toNode))
                nearest = nearest < 0 ? col[l.toNode] - 1 : std::min(nearest, col[l.toNode] - 1);
        if (nearest > col[n->id]) col[n->id] = nearest;
    }

    int maxCol = 0;
    for (auto& [id, c] : col) maxCol = std::max(maxCol, c);
    std::vector<std::vector<Node*>> columns(maxCol + 1);
    for (Node* n : nodes) columns[col[n->id]].push_back(n);

    // Column widths, so wide nodes (reroutes are narrow) don't overlap the next column.
    std::vector<float> colX(maxCol + 1, minX);
    for (int c = 1; c <= maxCol; ++c) {
        float w = 0;
        for (Node* n : columns[c - 1]) w = std::max(w, size(*n).w);
        colX[c] = colX[c - 1] + w + kLayoutGapX;
    }

    // Order each column by the mean height of its inputs (barycenter), falling back to the node's
    // current height, so the layout keeps the user's rough top-to-bottom order.
    std::map<int, float> key;
    bool moved = false;
    for (int c = 0; c <= maxCol; ++c) {
        auto& column = columns[c];
        for (Node* n : column) {
            float sum = 0;
            int cnt = 0;
            for (const Link& l : g.links())
                if (l.toNode == n->id && key.count(l.fromNode)) sum += key[l.fromNode], ++cnt;
            key[n->id] = cnt ? sum / cnt : n->y;
        }
        std::stable_sort(column.begin(), column.end(),
                         [&](Node* a, Node* b) { return key[a->id] < key[b->id]; });
        float y = minY;
        for (Node* n : column) {
            const float nx = std::round(colX[c]), ny = std::round(y);
            if (nx != n->x || ny != n->y) moved = true;
            n->x = nx, n->y = ny;
            // Re-key by the final centre so the next column follows the laid-out positions.
            key[n->id] = y + size(*n).h * 0.5f;
            y += size(*n).h + kLayoutGapY;
        }
    }
    return moved;
}
