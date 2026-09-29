#pragma once
#include "graph/Graph.h"

class GroupNode;

// Edits one param with the widget matching its kind. Returns true when the value changed.
// `compact` hides the label (used for inline widgets inside nodes).
bool editParam(Node& node, int paramIndex, float width, bool compact);

// Side panel with every param of the selected node. Returns true when anything changed.
// owner: the group whose inside is being edited (null at the root); its interface is editable
// when its Group Input / Output node is selected.
bool drawInspector(Graph& g, int selectedNode, GroupNode* owner, Graph* ownerParent);

// Name and input/output pin list of a group. `outer` is the graph containing the group node.
bool drawGroupInterface(GroupNode& group, Graph& outer);
