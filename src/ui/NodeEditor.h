#pragma once
#include <unordered_map>
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
//         drag a node onto a wire = insert it (committed on release), Alt+drag = pull a node out
//         of its chain, Ctrl+click node = preview (Ctrl+Shift+click cycles its outputs),
//         Shift+click node = add/remove from selection, right-click = context / add menu,
//         Ctrl+right-drag = cut wires, Shift+right-drag = add reroutes on wires.
// Keys:   Delete / X (reconnects around the node; Alt+Delete doesn't), Ctrl+C / Ctrl+V,
//         Ctrl+D duplicate, Shift+D duplicate and move, G move, H collapse, M mute, F make links,
//         L / Shift+L select upstream / downstream, F2 rename, Ctrl+A select all,
//         Home frame all, . frame selected, Alt+P remove from frame, Shift+A add menu.
class NodeEditor {
public:
    struct Result {
        bool evalChanged = false;     // graph topology or params changed -> re-evaluate
        bool docChanged = false;      // anything that should mark the project modified
        bool previewChanged = false;  // Ctrl+click toggled the preview node
        int enterGroup = 0;           // Tab / double-click on a group node: open it
        bool exitGroup = false;       // Tab with no group selected: go up one level
        int openViewer = 0;           // node menu "Open in New Viewer": pin this node in a new viewer
        // Find Node picked a node inside nested groups: open findPath (group ids below the graph
        // being drawn), then select and frame findNode there.
        std::vector<int> findPath;
        int findNode = 0;
        // Node menu > Mask Selected Nodes: run these nodes' edit through a new mask of this kind
        // (recipes::MaskKind), done by the App so it can select the mask. -1: not asked.
        int maskSelection = -1;
        std::vector<int> maskNodes;
    };

    // selected: the single selected node (0 if none or several). preview: node shown on the right.
    // previewPin: which output of the preview node is shown (Ctrl+Shift+click cycles it).
    Result draw(Graph& g, int& selected, int& preview, int& previewPin);

    // The graph was replaced (new/open/undo): drop selection and in-flight interactions.
    void onGraphReplaced(bool frame);  // also clears the active value field
    void frameAll() { fitFrames_ = 1; }
    // Ctrl+F: search this graph's nodes and those of the groups inside it by label or name, then
    // select and frame the pick. Opened next frame from the canvas, as a popup must be opened in
    // the ID scope that draws it.
    void openFind() { findRequested_ = true; }
    // Frame the selection on the next draw (after the App switched to a group the size is known).
    void frameSelectionNext() { frameSelectionNext_ = true; }
    // Blender's "Node Timings" overlay: how long each node took when it last ran, drawn above it.
    // Keyed by node id of the graph being drawn; empty hides the labels.
    // Nodes that ran on the GPU are marked so.
    void setTimings(std::unordered_map<int, double> t, std::unordered_map<int, bool> gpu = {}) {
        timings_ = std::move(t);
        gpuNodes_ = std::move(gpu);
    }
    void select(int nodeId);

    // Places a node so its title bar sits at a screen position (uses the current view).
    void placeAtScreen(Node& n, ImVec2 screen) const;
    ImVec2 canvasCenter() const { return ImVec2(origin_.x + size_.x * 0.5f, origin_.y + size_.y * 0.4f); }

    // True while the user is mid-gesture (dragging, editing a value), so undo snapshots wait.
    bool showTimings = true;
    // The graph drawn is a group's contents: the Add menu offers the group's Value Input / Output.
    bool insideGroup = false;
    bool interacting() const { return mode_ != Mode::None || editing_.node != 0 || activeNode_ != 0; }

    bool duplicateSelection(Graph& g);
    void openSwapMenu(const Graph& g);  // Shift+S: pick a type to replace the selected nodes with
    bool groupSelection(Graph& g);    // Ctrl+G
    bool ungroupSelection(Graph& g);  // Ctrl+Alt+G
    // Shift+P: tidy the selection (2+ nodes) or the whole graph into columns (Blender's Node Arrange).
    bool arrange(Graph& g);
    bool frameSelection(Graph& g);    // Ctrl+J: frame around the selection (or an empty frame)
    // Moves the selected nodes into frame `frameId` (growing it to fit), or out of their frames when
    // 0 (Alt+P). Membership is geometric (a node belongs to the smallest frame holding its centre).
    bool moveSelectionToFrame(Graph& g, int frameId);
    int frameOf(const Graph& g, const Node& n) const;
    int selectedGroup(const Graph& g) const;  // the single selected group node, or 0
    int selectedFrame() const { return selectedFrame_; }
    bool deleteSelection(Graph& g, int& preview, bool reconnect = true);
    void copySelection(const Graph& g);
    bool paste(Graph& g);
    bool toggleMute(Graph& g);
    bool toggleCollapse(Graph& g);
    bool resetSelection(Graph& g);  // Reset to Defaults: the selected nodes' params (not their files)
    // F (Blender's Make Links): wires the selected nodes left to right. Afterwards 1 steps the last
    // wire's output, 2 its input and F again both, until the selection changes, a click or Esc.
    bool makeLinks(Graph& g);
    bool cycleLink(Graph& g, int what);  // 0 next output/input pair, 1 next output, 2 next input
    // Alt+D: removes the wires between the selected nodes (those with both ends selected),
    // leaving their wires to other nodes alone.
    bool detachLinks(Graph& g);
    // Alt+S (Node Wrangler's Swap Links): two selected nodes trade places in the wiring (and on
    // the canvas); one selected node swaps its two wired inputs (Mix's A and B), or moves its one
    // wire to the next input that takes it.
    bool swapLinks(Graph& g);
    void beginGrab(Graph& g);
    bool hasSelection() const { return !selection_.empty() || selectedLink_ != 0; }

    nlohmann::json viewState() const;
    void setViewState(const nlohmann::json& j);

private:
    std::unordered_map<int, double> timings_;
    std::unordered_map<int, bool> gpuNodes_;
    enum class Mode { None, Pan, BoxSelect, PressNode, DragNodes, DragLink, DragFrame, ResizeFrame, Grab, Knife, RerouteCut };

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
    void syncOrder(Graph& g);

    int hitNode(const Graph& g, ImVec2 p) const;
    PinRef hitPin(const Graph& g, ImVec2 p) const;
    int hitLink(const Graph& g, ImVec2 p) const;
    void linkEnds(const Graph& g, const Link& l, ImVec2& a, ImVec2& b) const;

    void drawGrid(ImDrawList* dl) const;
    void drawFrames(ImDrawList* dl, const Graph& g) const;
    int hitFrameTitle(const Graph& g, ImVec2 p) const;
    int hitFrameCorner(const Graph& g, ImVec2 p) const;
    void drawFrameMenu(Graph& g, Result& r);
    void drawLinks(ImDrawList* dl, const Graph& g) const;
    bool drawNode(ImDrawList* dl, Graph& g, Node& n, int preview, Result& r);
    bool drawValueBox(Node& n, int param, const ImRect& box, ImDrawList* dl, const char* label);
    bool drawParamRow(ImDrawList* dl, Node& n, int param, const ImRect& box, bool canInteract);
    void updateInsertCandidate(const Graph& g);
    void finishLinkDrag(Graph& g, Result& r);
    void doFrame(const Graph& g);
    void drawAddMenu(Graph& g, Result& r);
    void drawNodeMenu(Graph& g, int& preview, Result& r);
    void drawRenamePopup(Graph& g, Result& r);
    void drawPresetPopup(const Graph& g);
    // The selection's nodes and the wires among them (the clipboard's and presets' format).
    nlohmann::json selectionJson(const Graph& g) const;
    // Adds clip's nodes and wires with their top-left corner at grid position `at`; selects them.
    bool insertClip(Graph& g, const nlohmann::json& clip, ImVec2 at);
    void drawFindMenu(Graph& g, Result& r);
    void finishDragNodes(Graph& g, Result& r);  // splice + auto-offset
    void selectLinked(const Graph& g, bool downstream);
    void frameSelected(const Graph& g);

    // view
    ImVec2 origin_{}, size_{};
    ImVec2 pan_{40, 40};
    float zoom_ = 1.0f;
    int fitFrames_ = 3;  // frames left to re-fit (docked windows settle their size over a few frames)

    // selection / ordering
    std::set<int> selection_;
    // The wire F made last, which 1 / 2 / F step through other pins for (see cycleLink). replaced:
    // the wire F displaced from toPin, put back when the cycle moves off that input.
    struct LinkCycle {
        int from = 0, fromPin = 0, to = 0, toPin = 0;
        int replacedFrom = 0, replacedFromPin = 0, replacedTo = -1;
        std::set<int> selection;
    } cycle_;
    void drawLinkCycleHint(const Graph& g);
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
    // A value box under the mouse this frame: Backspace resets it instead of deleting the node.
    bool valueHovered_ = false;
    int valueMenuNode_ = 0, valueMenuParam_ = -1;  // right-clicked value box (its menu)
    bool openValueMenu_ = false;

    int previewPin_ = 0;
    std::vector<ImVec2> knife_;  // screen points of a cut / reroute gesture
    int renameNode_ = 0;
    char renameBuf_[128] = {};

    // frames
    int selectedFrame_ = 0;
    float frameStart_[4] = {};               // x, y, w, h at drag start
    std::map<int, ImVec2> frameNodes_;       // nodes carried along while dragging a frame
    int menuFrame_ = 0;
    char frameLabel_[128] = {};

    // menus
    ImVec2 menuPos_{};
    PinRef menuConnect_;  // wire dragged into empty space: connect the chosen node to this pin
    int menuNode_ = 0;
    std::vector<int> swapTargets_;  // add menu opened as Swap (Shift+S): replace these nodes instead
    char search_[64] = {};
    int searchSel_ = 0;  // highlighted search result (Up/Down, Enter)
    std::string wheelMenu_;  // the Add submenu the wheel steps through, and its highlighted row
    int wheelSel_ = -1;
    bool findRequested_ = false;
    char presetName_[64] = {};
    std::string presetStatus_;      // the last save's error, shown in the popup
    std::vector<std::string> presetNames_;  // listed when the Add menu opens
    bool frameSelectionNext_ = false;
    // Frames left before checking that a restored view (setViewState) shows any node; when it
    // shows none (saved with another panel size, or nodes moved), the graph is framed instead.
    int checkVisible_ = 0;
};

ImU32 pinColor(PinType t);
