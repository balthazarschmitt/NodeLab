#include "ui/NodeEditor.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include <imnodes.h>

#include "graph/NodeRegistry.h"
#include "ui/Inspector.h"

ImU32 pinColor(PinType t) {
    switch (t) {
        case PinType::Image: return IM_COL32(236, 184, 72, 255);    // amber
        case PinType::Channel: return IM_COL32(176, 176, 190, 255); // gray
        case PinType::Number: return IM_COL32(96, 160, 236, 255);   // blue
    }
    return IM_COL32_WHITE;
}

static bool containsNoCase(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    auto lower = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    };
    lower(h);
    lower(n);
    return h.find(n) != std::string::npos;
}

NodeEditor::Result NodeEditor::draw(Graph& g, int& selected, int& preview) {
    Result r;
    ImVec2 winPos = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    canvasCenter_ = ImVec2(winPos.x + avail.x * 0.5f, winPos.y + avail.y * 0.4f);

    ImNodes::BeginNodeEditor();

    if (fitPending_ && !g.nodes().empty()) {
        float minX = 1e9f, minY = 1e9f;
        for (const auto& [id, n] : g.nodes()) {
            minX = std::min(minX, n->x);
            minY = std::min(minY, n->y);
        }
        ImNodes::EditorContextResetPanning(ImVec2(30.0f - minX, 30.0f - minY));
        fitPending_ = false;
    }

    for (const auto& [id, nodePtr] : g.nodes()) {
        Node& n = *nodePtr;
        const NodeInfo& info = n.info();

        if (!placed_.count(id)) {
            if (auto it = pendingScreenPos_.find(id); it != pendingScreenPos_.end()) {
                ImNodes::SetNodeScreenSpacePos(id, it->second);
                pendingScreenPos_.erase(it);
            } else {
                ImNodes::SetNodeGridSpacePos(id, ImVec2(n.x, n.y));
            }
            placed_.insert(id);
        }

        const bool isPreview = (id == preview);
        if (isPreview) {
            ImNodes::PushColorStyle(ImNodesCol_TitleBar, IM_COL32(150, 90, 30, 255));
            ImNodes::PushColorStyle(ImNodesCol_TitleBarHovered, IM_COL32(175, 105, 35, 255));
            ImNodes::PushColorStyle(ImNodesCol_TitleBarSelected, IM_COL32(195, 120, 40, 255));
        }

        ImGui::PushID(id);
        ImNodes::BeginNode(id);

        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(info.displayName.c_str());
        if (isPreview) {
            ImGui::SameLine();
            ImGui::TextUnformatted("[preview]");
        }
        ImNodes::EndNodeTitleBar();

        // Inputs; unconnected inputs with a backing param get an inline drag widget.
        for (int p = 0; p < int(info.inputs.size()); ++p) {
            const PinDesc& pin = info.inputs[p];
            ImNodes::PushColorStyle(ImNodesCol_Pin, pinColor(pin.type));
            // Drag-to-detach only on inputs (they hold at most one wire). On outputs it would steal
            // the existing wire instead of starting a new one, blocking fan-out.
            ImNodes::PushAttributeFlag(ImNodesAttributeFlags_EnableLinkDetachWithDragClick);
            ImNodes::BeginInputAttribute(attrId(id, p, false),
                                         pin.type == PinType::Image ? ImNodesPinShape_QuadFilled
                                                                    : ImNodesPinShape_CircleFilled);
            ImGui::TextUnformatted(pin.name.c_str());
            if (pin.fallbackParam >= 0 && !g.inputLink(id, p)) {
                ImGui::SameLine();
                if (editParam(n, pin.fallbackParam, 70.0f, true)) {
                    r.evalChanged = r.docChanged = true;
                }
            }
            ImNodes::EndInputAttribute();
            ImNodes::PopAttributeFlag();
            ImNodes::PopColorStyle();
        }

        // Path params are shown inline too (the file name), so Image Input is useful without the inspector.
        for (int i = 0; i < int(info.params.size()); ++i) {
            if (info.params[i].kind != ParamKind::Path) continue;
            if (editParam(n, i, 120.0f, true)) r.evalChanged = r.docChanged = true;
        }

        for (int p = 0; p < int(info.outputs.size()); ++p) {
            const PinDesc& pin = info.outputs[p];
            ImNodes::PushColorStyle(ImNodesCol_Pin, pinColor(pin.type));
            ImNodes::BeginOutputAttribute(attrId(id, p, true),
                                          pin.type == PinType::Image ? ImNodesPinShape_QuadFilled
                                                                     : ImNodesPinShape_CircleFilled);
            const float labelW = ImGui::CalcTextSize(pin.name.c_str()).x;
            ImGui::Indent(std::max(0.0f, 90.0f - labelW));
            ImGui::TextUnformatted(pin.name.c_str());
            ImNodes::EndOutputAttribute();
            ImNodes::PopColorStyle();
        }

        ImNodes::EndNode();
        ImGui::PopID();
        if (isPreview) {
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
            ImNodes::PopColorStyle();
        }
    }

    for (const Link& l : g.links()) {
        Node* from = g.find(l.fromNode);
        if (!from) continue;
        ImNodes::PushColorStyle(ImNodesCol_Link, pinColor(from->info().outputs[l.fromPin].type));
        ImNodes::Link(l.id, attrId(l.fromNode, l.fromPin, true), attrId(l.toNode, l.toPin, false));
        ImNodes::PopColorStyle();
    }

    ImNodes::MiniMap(0.15f, ImNodesMiniMapLocation_BottomRight);
    ImNodes::EndNodeEditor();

    // ---- interactions (must come after EndNodeEditor)

    // Selecting by id requires imnodes to know the node, i.e. it must have been drawn at least once;
    // this runs after the node loop, so a node added last frame now exists.
    if (pendingSelect_ && g.find(pendingSelect_)) {
        ImNodes::ClearNodeSelection();
        ImNodes::SelectNode(pendingSelect_);
        pendingSelect_ = 0;
    }
    int startAttr = 0, endAttr = 0;
    if (ImNodes::IsLinkCreated(&startAttr, &endAttr)) {
        int a = startAttr, b = endAttr;
        if ((a & 128) == 0) std::swap(a, b);  // make `a` the output side
        if ((a & 128) && !(b & 128)) {
            if (g.connect(a / 256, a % 128, b / 256, b % 128)) r.evalChanged = r.docChanged = true;
        }
    }
    int destroyed = 0;
    if (ImNodes::IsLinkDestroyed(&destroyed)) {
        g.removeLink(destroyed);
        r.evalChanged = r.docChanged = true;
    }

    const ImGuiIO& io = ImGui::GetIO();
    int hoveredNode = 0;
    const bool nodeHovered = ImNodes::IsNodeHovered(&hoveredNode);
    if (nodeHovered && io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        preview = (preview == hoveredNode) ? 0 : hoveredNode;
        r.previewChanged = true;
    }

    // Delete selection
    const bool editorFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    if (editorFocused && !io.WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
        int nl = ImNodes::NumSelectedLinks();
        if (nl > 0) {
            std::vector<int> ids(nl);
            ImNodes::GetSelectedLinks(ids.data());
            for (int id : ids) g.removeLink(id);
            r.evalChanged = r.docChanged = true;
        }
        int nn = ImNodes::NumSelectedNodes();
        if (nn > 0) {
            std::vector<int> ids(nn);
            ImNodes::GetSelectedNodes(ids.data());
            for (int id : ids) {
                g.removeNode(id);
                placed_.erase(id);
                if (preview == id) preview = 0;
            }
            r.evalChanged = r.docChanged = true;
        }
        ImNodes::ClearNodeSelection();
        ImNodes::ClearLinkSelection();
    }

    // Selection -> inspector
    int ns = ImNodes::NumSelectedNodes();
    if (ns == 1) {
        int id = 0;
        ImNodes::GetSelectedNodes(&id);
        selected = g.find(id) ? id : 0;
    } else {
        selected = 0;
    }

    // Persist positions and detect moves.
    for (const auto& [id, n] : g.nodes()) {
        ImVec2 p = ImNodes::GetNodeGridSpacePos(id);
        if (p.x != n->x || p.y != n->y) {
            n->x = p.x;
            n->y = p.y;
            r.docChanged = true;
        }
    }

    // Right-click on empty canvas -> add menu
    // Not ImNodes::IsEditorHovered(): it checks IsWindowHovered() of the *current* window, which after
    // EndNodeEditor is this container, not imnodes' inner canvas child, so it is always false here.
    const bool canvasHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    int hoveredLink = 0;
    if (canvasHovered && !nodeHovered && !ImNodes::IsLinkHovered(&hoveredLink) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        addMenuPos_ = io.MousePos;
        search_[0] = '\0';
        ImGui::OpenPopup("AddNode");
    }
    drawAddMenu(g, r);
    return r;
}

void NodeEditor::drawAddMenu(Graph& g, Result& r) {
    if (!ImGui::BeginPopup("AddNode")) return;
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##search", "Search nodes...", search_, sizeof(search_));

    const auto& reg = NodeRegistry::instance();
    std::string chosen;
    if (search_[0]) {
        for (const auto& type : reg.types()) {
            const NodeInfo* inf = reg.find(type);
            if (containsNoCase(inf->displayName, search_) || containsNoCase(inf->category, search_)) {
                if (ImGui::MenuItem(inf->displayName.c_str(), inf->category.c_str())) chosen = type;
            }
        }
        // Enter picks the first match.
        if (chosen.empty() && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            for (const auto& type : reg.types()) {
                const NodeInfo* inf = reg.find(type);
                if (containsNoCase(inf->displayName, search_) || containsNoCase(inf->category, search_)) {
                    chosen = type;
                    break;
                }
            }
        }
    } else {
        std::vector<std::string> cats;
        for (const auto& type : reg.types()) {
            const std::string& c = reg.find(type)->category;
            if (std::find(cats.begin(), cats.end(), c) == cats.end()) cats.push_back(c);
        }
        for (const auto& c : cats) {
            if (ImGui::BeginMenu(c.c_str())) {
                for (const auto& type : reg.types()) {
                    const NodeInfo* inf = reg.find(type);
                    if (inf->category == c && ImGui::MenuItem(inf->displayName.c_str())) chosen = type;
                }
                ImGui::EndMenu();
            }
        }
    }

    if (!chosen.empty()) {
        if (Node* n = g.addNode(chosen)) {
            placeAtScreen(n->id, addMenuPos_);
            pendingSelect_ = n->id;
            r.evalChanged = r.docChanged = true;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
