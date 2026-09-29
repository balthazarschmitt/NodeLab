#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include "graph/Graph.h"

// Node graph canvas drawn directly with ImGui draw lists (pan, zoom, selection, wiring).
//
// Mouse:  left-drag empty = pan, Shift+left-drag empty = box select, middle-drag = pan,
//         wheel = zoom, drag pin = wire (drop on empty space = add-node menu that connects),
//         drag a node onto a wire = insert it (committed on release), Ctrl+click node = preview,
//         Shift+click node = add/remove from selection, right-click = context / add menu.
// Keys:   Delete, Ctrl+D duplicate, Ctrl+A select all, F frame all.
class NodeEditor {
public:
    struct Result {
        bool evalChanged = false;     // graph topology or params changed -> re-evaluate
        bool docChanged = false;      // anything that should mark the project modified
        bool previewChanged = false;  // Ctrl+click toggled the preview node
    };

    // selected: the single selected node (0 if none or several). preview: node shown on the right.
    Result draw(Graph& g, int& selected, int& preview);

    // The graph was replaced (new/open/undo): drop selection and in-flight interactions.
    void onGraphReplaced(bool frame);  // also clears the active value field
    void frameAll() { fitPending_ = true; }
    void select(int nodeId);

    // Places a node so its title bar sits at a screen position (uses the current view).
    void placeAtScreen(Node& n, ImVec2 screen) const;
    ImVec2 canvasCenter() const { return ImVec2(origin_.x + size_.x * 0.5f, origin_.y + size_.y * 0.4f); }

    // True while the user is mid-gesture (dragging, editing a value), so undo snapshots wait.
    bool interacting() const { return mode_ != Mode::None || editing_.node != 0 || activeNode_ != 0; }

    bool duplicateSelection(Graph& g);
    bool deleteSelection(Graph& g, int& preview);
    bool hasSelection() const { return !selection_.empty() || selectedLink_ != 0; }

    nlohmann::json viewState() const;
    void setViewState(const nlohmann::json& j);

private:
    enum class Mode { None, Pan, BoxSelect, PressNode, DragNodes, DragLink };

    struct PinRef {
        int node = 0, pin = 0;
        bool output = false;
        bool valid() const { return node != 0; }
    };

    struct Layout {
        ImVec2 min, max;
        float titleH = 0;
        std::vector<ImVec2> inPins, outPins;
        std::vector<ImRect> valueBoxes;  // per input; zero-size when no inline value
        std::vector<ImRect> paramBoxes;  // per param row on the body; zero-size if not shown
    };

    Layout layoutFor(const Node& n) const;
    ImVec2 toScreen(ImVec2 grid) const;
    ImVec2 toGrid(ImVec2 screen) const;
    void syncOrder(const Graph& g);

    int hitNode(const Graph& g, ImVec2 p) const;
    PinRef hitPin(const Graph& g, ImVec2 p) const;
    int hitLink(const Graph& g, ImVec2 p) const;
    void linkEnds(const Graph& g, const Link& l, ImVec2& a, ImVec2& b) const;

    void drawGrid(ImDrawList* dl) const;
    void drawLinks(ImDrawList* dl, const Graph& g) const;
    bool drawNode(ImDrawList* dl, Graph& g, Node& n, int preview, Result& r);
    bool drawValueBox(Node& n, int param, const ImRect& box, ImDrawList* dl, const char* label);
    bool drawParamRow(ImDrawList* dl, Node& n, int param, const ImRect& box, bool canInteract);
    void updateInsertCandidate(const Graph& g);
    void finishLinkDrag(Graph& g, Result& r);
    void doFrame(const Graph& g);
    void drawAddMenu(Graph& g, Result& r);
    void drawNodeMenu(Graph& g, int& preview, Result& r);

    // view
    ImVec2 origin_{}, size_{};
    ImVec2 pan_{40, 40};
    float zoom_ = 1.0f;
    bool fitPending_ = true;

    // selection / ordering
    std::set<int> selection_;
    int selectedLink_ = 0;
    std::vector<int> order_;  // draw order, last = topmost
    int pendingSelect_ = 0;

    // gestures
    Mode mode_ = Mode::None;
    ImVec2 pressPos_{};
    std::map<int, ImVec2> dragStart_;  // node -> grid pos at drag start
    int pressNode_ = 0;
    PinRef linkFrom_;
    bool linkDetached_ = false;
    PinRef hoverPin_;

    // insert-on-wire candidate while dragging a single unconnected node
    int insertLink_ = 0, insertIn_ = -1, insertOut_ = -1;

    // inline value editing (text entry)
    struct EditState {
        int node = 0, param = -1;
        int frames = 0;       // frames since editing started
        bool wasActive = false;
    } editing_;
    // Value field currently being dragged. It must keep being submitted even when the mouse leaves
    // its node, otherwise ImGui drops the active item mid-drag.
    int activeNode_ = 0, activeParam_ = -1;
    int enumNode_ = 0, enumParam_ = -1;  // node/param whose dropdown popup is open

    // menus
    ImVec2 menuPos_{};
    PinRef menuConnect_;  // wire dragged into empty space: connect the chosen node to this pin
    int menuNode_ = 0;
    char search_[64] = {};
};

ImU32 pinColor(PinType t);
