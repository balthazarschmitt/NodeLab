#pragma once
#include <map>
#include <set>
#include <string>

#include <imgui.h>

#include "graph/Graph.h"

class NodeEditor {
public:
    struct Result {
        bool evalChanged = false;     // graph topology or params changed -> re-evaluate
        bool docChanged = false;      // anything that should mark the project modified
        bool previewChanged = false;  // Ctrl+click toggled the preview node
    };

    // selected: the single selected node (0 if none). preview: Ctrl+clicked node (0 = Output).
    Result draw(Graph& g, int& selected, int& preview);

    // Call after the graph is replaced (new/open) so node positions are re-applied.
    void resetPlacement() {
        placed_.clear();
        pendingScreenPos_.clear();
        fitPending_ = true;
    }
    // Place a node the app just created at a screen position on the next frame.
    void placeAtScreen(int nodeId, ImVec2 pos) { pendingScreenPos_[nodeId] = pos; }
    void select(int nodeId) { pendingSelect_ = nodeId; }

    // Screen-space center of the canvas, for placing nodes added from menus.
    ImVec2 canvasCenter() const { return canvasCenter_; }

    static int attrId(int node, int pin, bool output) { return node * 256 + (output ? 128 : 0) + pin; }

private:
    void drawAddMenu(Graph& g, Result& r);

    std::set<int> placed_;
    std::map<int, ImVec2> pendingScreenPos_;
    int pendingSelect_ = 0;
    bool fitPending_ = true;  // pan so the graph's top-left is in view
    ImVec2 addMenuPos_{};
    ImVec2 canvasCenter_{};
    char search_[64] = {};
};

ImU32 pinColor(PinType t);
