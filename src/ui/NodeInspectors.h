#pragma once
#include <functional>

#include "graph/Node.h"

// Draws one param's Inspector row (handles wired params); returns true when it changed.
using ParamRow = std::function<bool(int)>;

// Custom Inspector layout for nodes that have one (Basic, Color Mixer, Color Grading, Brush Mask),
// plus a usage hint for nodes with on-image controls. Returns false when the default param list
// should be drawn instead. `changed` is set when a custom widget edited the node.
bool drawNodeInspector(Node& n, const ParamRow& row, bool& changed);

// Set by Basic's Auto button to that node's id; the App works out the settings from the image
// arriving at it (it can evaluate the graph) and clears it.
extern int autoToneRequest;
