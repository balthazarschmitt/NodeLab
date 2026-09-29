#pragma once
#include "graph/Graph.h"

// Edits one param with the widget matching its kind. Returns true when the value changed.
// `compact` hides the label (used for inline widgets inside nodes).
bool editParam(Node& node, int paramIndex, float width, bool compact);

// Side panel with every param of the selected node. Returns true when anything changed.
bool drawInspector(Graph& g, int selectedNode);
