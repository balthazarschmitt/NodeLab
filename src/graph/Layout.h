#pragma once
#include <functional>
#include <set>

#include "graph/Graph.h"

// Automatic node spacing, in grid units. The editor supplies each node's drawn size, which the
// graph itself doesn't know.
struct NodeSize {
    float w = 0, h = 0;
};
using NodeSizeFn = std::function<NodeSize(const Node&)>;

constexpr float kLayoutGapX = 60.0f;  // between columns
constexpr float kLayoutGapY = 30.0f;  // between nodes in a column

// Pushes nodes that overlap any of `anchors` out of the way, along whichever axis needs the
// smaller move. A sideways push carries the pushed node's downstream (or upstream, when pushed
// left) chain along so wires keep flowing left to right. Anchors themselves never move. Returns
// true if anything moved.
bool spaceOut(Graph& g, const std::set<int>& anchors, const NodeSizeFn& size);

// Tidy layered layout of `ids` (Blender's Node Arrange): each node goes one column right of its
// furthest upstream node within the set, columns are stacked top-down ordered by where their
// inputs come from, and the block keeps its top-left corner. Returns true if anything moved.
bool arrangeNodes(Graph& g, const std::set<int>& ids, const NodeSizeFn& size);
