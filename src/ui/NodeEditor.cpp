#include "ui/NodeEditor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <imgui_internal.h>

#include "core/Curve.h"
#include "core/Ramp.h"
#include "graph/NodeRegistry.h"
#include "nodes/group/GroupNodes.h"
#include "io/Paths.h"
#include "ui/FileDialog.h"

namespace {

// Node geometry in grid units (multiplied by zoom on screen).
constexpr float kNodeW = 200.0f;
constexpr float kTitleH = 26.0f;
constexpr float kRowH = 24.0f;
constexpr float kPad = 6.0f;
constexpr float kPinR = 5.0f;
constexpr float kMinZoom = 0.25f, kMaxZoom = 2.0f;

const char* kImageFilter = "Images|*.png;*.jpg;*.jpeg;*.bmp;*.tga|All files|*.*";

bool containsNoCase(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    auto lower = [](std::string& s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    };
    lower(h);
    lower(n);
    return h.find(n) != std::string::npos;
}

ImU32 categoryColor(const std::string& cat) {
    if (cat == "Input / Output") return IM_COL32(56, 108, 78, 255);
    if (cat == "Color") return IM_COL32(64, 88, 148, 255);
    if (cat == "Mix") return IM_COL32(104, 74, 136, 255);
    if (cat == "Converter") return IM_COL32(42, 108, 118, 255);
    if (cat == "Filter") return IM_COL32(120, 70, 60, 255);
    if (cat == "Transform") return IM_COL32(110, 96, 48, 255);
    if (cat == "Matte") return IM_COL32(70, 70, 110, 255);
    if (cat == "Texture") return IM_COL32(126, 76, 104, 255);
    if (cat == "Utility") return IM_COL32(70, 76, 84, 255);
    if (cat == "Group") return IM_COL32(40, 120, 60, 255);
    return IM_COL32(80, 80, 92, 255);
}

bool pinBacked(const NodeInfo& info, int param) {
    for (const auto& p : info.inputs)
        if (p.fallbackParam == param) return true;
    return false;
}

// Rows a param occupies on the node body (0 = inspector only / shown on its input pin).
int paramRows(const NodeInfo& info, int i) {
    switch (info.params[i].kind) {
        case ParamKind::Float: return pinBacked(info, i) ? 0 : 1;
        case ParamKind::Path:
        case ParamKind::Enum:
        case ParamKind::Bool:
        case ParamKind::Text:
        case ParamKind::Ramp:
        case ParamKind::Color:
        case ParamKind::SavePath: return 1;
        case ParamKind::Curve: return 4;
        case ParamKind::Int: return 0;
    }
    return 0;
}

constexpr float kRerouteSize = 20.0f;
bool isReroute(const Node& n) { return n.info().type == "util.reroute"; }

float nodeHeightGrid(const Node& n) {
    if (isReroute(n)) return kRerouteSize;
    if (n.collapsed) {
        int pins = int(std::max(n.info().inputs.size(), n.info().outputs.size()));
        return std::max(kTitleH + 6.0f, kTitleH * 0.5f + pins * 8.0f + 8.0f);
    }
    const NodeInfo& info = n.info();
    int rows = int(info.inputs.size() + info.outputs.size());
    for (int i = 0; i < int(info.params.size()); ++i) rows += paramRows(info, i);
    return kTitleH + kPad * 2 + rows * kRowH;
}

void bezierPoints(ImVec2 a, ImVec2 b, float zoom, ImVec2& c1, ImVec2& c2) {
    float d = std::max(std::fabs(b.x - a.x) * 0.5f, 40.0f * zoom);
    c1 = ImVec2(a.x + d, a.y);
    c2 = ImVec2(b.x - d, b.y);
}

// Index of the input (inputs=true) or output pin best matching `other`, or -1.
// Inputs: other is the source type; exact type first, then anything convertible. Free inputs preferred.
int pickPin(const Graph& g, const Node& n, bool inputs, PinType other) {
    const auto& pins = inputs ? n.info().inputs : n.info().outputs;
    for (int pass = 0; pass < 4; ++pass) {
        const bool exact = pass % 2 == 0, needFree = inputs && pass < 2;
        for (int i = 0; i < int(pins.size()); ++i) {
            if (needFree && g.inputLink(n.id, i)) continue;
            bool ok = exact ? pins[i].type == other
                            : (inputs ? canConvert(other, pins[i].type) : canConvert(pins[i].type, other));
            if (ok) return i;
        }
    }
    return -1;
}

// Slider-like field: fill proportional to the value, label left, value right.
void drawValueField(ImDrawList* dl, const ImRect& box, const char* label, float v, const ParamDesc& d, float fs,
                    float zoom, bool hovered) {
    const float frac = d.max > d.min ? std::clamp((v - d.min) / (d.max - d.min), 0.0f, 1.0f) : 0.0f;
    const float round = 3.0f * zoom;
    dl->AddRectFilled(box.Min, box.Max, hovered ? IM_COL32(34, 34, 40, 255) : IM_COL32(26, 26, 30, 255), round);
    dl->AddRectFilled(box.Min, ImVec2(box.Min.x + box.GetWidth() * frac, box.Max.y),
                      hovered ? IM_COL32(78, 108, 170, 255) : IM_COL32(60, 88, 146, 255), round);
    if (fs < 6.0f) return;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    ImFont* font = ImGui::GetFont();
    const ImVec2 vs = font->CalcTextSizeA(fs, FLT_MAX, 0, buf);
    const float ty = box.GetCenter().y - fs * 0.5f;
    const float pad = 6.0f * zoom;
    ImVec4 clip(box.Min.x, box.Min.y, box.Max.x - vs.x - pad * 2, box.Max.y);
    dl->AddText(font, fs, ImVec2(box.Min.x + pad, ty), IM_COL32(222, 222, 228, 255), label, nullptr, 0.0f, &clip);
    dl->AddText(font, fs, ImVec2(box.Max.x - pad - vs.x, ty), IM_COL32(240, 240, 245, 255), buf);
}

}  // namespace

ImU32 pinColor(PinType t) {
    switch (t) {
        case PinType::Image: return IM_COL32(236, 184, 72, 255);    // amber
        case PinType::Channel: return IM_COL32(176, 176, 190, 255); // gray
        case PinType::Number: return IM_COL32(96, 160, 236, 255);   // blue
    }
    return IM_COL32_WHITE;
}

// ---------------------------------------------------------------- view helpers

ImVec2 NodeEditor::toScreen(ImVec2 p) const {
    return ImVec2(origin_.x + pan_.x + p.x * zoom_, origin_.y + pan_.y + p.y * zoom_);
}

ImVec2 NodeEditor::toGrid(ImVec2 p) const {
    return ImVec2((p.x - origin_.x - pan_.x) / zoom_, (p.y - origin_.y - pan_.y) / zoom_);
}

void NodeEditor::placeAtScreen(Node& n, ImVec2 screen) const {
    ImVec2 gp = toGrid(screen);
    n.x = std::round(gp.x - kNodeW * 0.5f);
    n.y = std::round(gp.y - kTitleH * 0.5f);
}

NodeEditor::Layout NodeEditor::layoutFor(const Node& n) const {
    const NodeInfo& info = n.info();
    const float z = zoom_;
    Layout L;
    L.min = toScreen(ImVec2(n.x, n.y));
    if (isReroute(n)) {
        const float s = kRerouteSize * z;
        L.max = ImVec2(L.min.x + s, L.min.y + s);
        L.titleH = 0;
        L.outPins.emplace_back(L.max.x - s * 0.25f, L.min.y + s * 0.5f);
        L.inPins.emplace_back(L.min.x + s * 0.25f, L.min.y + s * 0.5f);
        L.valueBoxes.push_back(ImRect());
        L.paramBoxes.resize(info.params.size());
        return L;
    }
    L.max = ImVec2(L.min.x + kNodeW * z, L.min.y + nodeHeightGrid(n) * z);
    L.titleH = kTitleH * z;
    if (n.collapsed) {
        // Pins stacked along the sides of the title bar.
        for (size_t i = 0; i < info.outputs.size(); ++i) L.outPins.emplace_back(L.max.x, L.min.y + (kTitleH * 0.5f + i * 8.0f) * z);
        for (size_t i = 0; i < info.inputs.size(); ++i) {
            L.inPins.emplace_back(L.min.x, L.min.y + (kTitleH * 0.5f + i * 8.0f) * z);
            L.valueBoxes.push_back(ImRect());
        }
        L.paramBoxes.resize(info.params.size());
        return L;
    }
    float y = L.min.y + (kTitleH + kPad) * z;
    const float row = kRowH * z;
    for (size_t i = 0; i < info.outputs.size(); ++i, y += row) L.outPins.emplace_back(L.max.x, y + row * 0.5f);
    L.paramBoxes.resize(info.params.size());
    for (int i = 0; i < int(info.params.size()); ++i) {
        int rows = paramRows(info, i);
        if (rows == 0) continue;
        L.paramBoxes[i] = ImRect(L.min.x + 10 * z, y + 2 * z, L.max.x - 10 * z, y + row * rows - 2 * z);
        y += row * rows;
    }
    for (size_t i = 0; i < info.inputs.size(); ++i, y += row) {
        L.inPins.emplace_back(L.min.x, y + row * 0.5f);
        ImRect box;
        if (info.inputs[i].fallbackParam >= 0 && info.params[info.inputs[i].fallbackParam].kind == ParamKind::Float)
            box = ImRect(L.min.x + 10 * z, y + 2 * z, L.max.x - 10 * z, y + row - 2 * z);
        L.valueBoxes.push_back(box);
    }
    return L;
}

void NodeEditor::syncOrder(Graph& g) {
    g.pruneInvalidLinks();  // safety net: never draw a wire to a pin that no longer exists
    std::erase_if(order_, [&](int id) { return !g.find(id); });
    for (const auto& [id, n] : g.nodes())
        if (std::find(order_.begin(), order_.end(), id) == order_.end()) order_.push_back(id);
    for (auto it = selection_.begin(); it != selection_.end();) it = g.find(*it) ? std::next(it) : selection_.erase(it);
    if (selectedLink_ && std::none_of(g.links().begin(), g.links().end(),
                                      [&](const Link& l) { return l.id == selectedLink_; }))
        selectedLink_ = 0;
}

void NodeEditor::doFrame(const Graph& g) {
    if (g.nodes().empty() || size_.x < 50 || size_.y < 50) {
        zoom_ = 1.0f;
        pan_ = ImVec2(40, 40);
        return;
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const auto& [id, n] : g.nodes()) {
        x0 = std::min(x0, n->x);
        y0 = std::min(y0, n->y);
        x1 = std::max(x1, n->x + kNodeW);
        y1 = std::max(y1, n->y + nodeHeightGrid(*n));
    }
    zoom_ = std::clamp(std::min((size_.x - 60) / (x1 - x0), (size_.y - 60) / (y1 - y0)), 0.3f, 1.0f);
    pan_ = ImVec2(size_.x * 0.5f - (x0 + x1) * 0.5f * zoom_, size_.y * 0.5f - (y0 + y1) * 0.5f * zoom_);
}

void NodeEditor::onGraphReplaced(bool frame) {
    selection_.clear();
    selectedLink_ = 0;
    order_.clear();
    mode_ = Mode::None;
    editing_ = {};
    selectedFrame_ = 0;
    activeNode_ = 0;
    activeParam_ = -1;
    insertLink_ = 0;
    hoverPin_ = {};
    if (frame) fitFrames_ = 3;
}

void NodeEditor::select(int nodeId) {
    selection_ = {nodeId};
    selectedLink_ = 0;
}

nlohmann::json NodeEditor::viewState() const { return {pan_.x, pan_.y, zoom_}; }

void NodeEditor::setViewState(const nlohmann::json& j) {
    if (!j.is_array() || j.size() != 3) return;
    pan_ = ImVec2(j[0].get<float>(), j[1].get<float>());
    zoom_ = std::clamp(j[2].get<float>(), kMinZoom, kMaxZoom);
    fitFrames_ = 0;
}

// ---------------------------------------------------------------- hit testing

int NodeEditor::hitNode(const Graph& g, ImVec2 p) const {
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        const Node* n = g.find(*it);
        if (!n) continue;
        Layout L = layoutFor(*n);
        if (ImRect(L.min, L.max).Contains(p)) return *it;
    }
    return 0;
}

NodeEditor::PinRef NodeEditor::hitPin(const Graph& g, ImVec2 p) const {
    const float r = std::max(8.0f, 9.0f * zoom_);
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        const Node* n = g.find(*it);
        if (!n) continue;
        Layout L = layoutFor(*n);
        for (int i = 0; i < int(L.outPins.size()); ++i)
            if (ImLengthSqr(L.outPins[i] - p) < r * r) return {*it, i, true};
        for (int i = 0; i < int(L.inPins.size()); ++i)
            if (ImLengthSqr(L.inPins[i] - p) < r * r) return {*it, i, false};
    }
    return {};
}

void NodeEditor::linkEnds(const Graph& g, const Link& l, ImVec2& a, ImVec2& b) const {
    a = layoutFor(*g.find(l.fromNode)).outPins[l.fromPin];
    b = layoutFor(*g.find(l.toNode)).inPins[l.toPin];
}

int NodeEditor::hitLink(const Graph& g, ImVec2 p) const {
    const float tol = std::max(5.0f, 5.0f * zoom_);
    for (const Link& l : g.links()) {
        ImVec2 a, b, c1, c2;
        linkEnds(g, l, a, b);
        bezierPoints(a, b, zoom_, c1, c2);
        ImVec2 closest = ImBezierCubicClosestPointCasteljau(a, c1, c2, b, p, 1.0f);
        if (ImLengthSqr(closest - p) < tol * tol) return l.id;
    }
    return 0;
}

// ---------------------------------------------------------------- drawing

void NodeEditor::drawGrid(ImDrawList* dl) const {
    float step = 24.0f * zoom_;
    while (step < 10.0f) step *= 4.0f;
    const ImU32 col = IM_COL32(44, 44, 52, 255);
    for (float x = std::fmod(pan_.x, step); x < size_.x; x += step)
        dl->AddLine(ImVec2(origin_.x + x, origin_.y), ImVec2(origin_.x + x, origin_.y + size_.y), col);
    for (float y = std::fmod(pan_.y, step); y < size_.y; y += step)
        dl->AddLine(ImVec2(origin_.x, origin_.y + y), ImVec2(origin_.x + size_.x, origin_.y + y), col);
}

void NodeEditor::drawLinks(ImDrawList* dl, const Graph& g) const {
    const float thick = std::max(1.5f, 2.5f * zoom_);
    for (const Link& l : g.links()) {
        ImVec2 a, b, c1, c2;
        linkEnds(g, l, a, b);
        bezierPoints(a, b, zoom_, c1, c2);
        ImU32 col = pinColor(g.find(l.fromNode)->info().outputs[l.fromPin].type);
        float t = thick;
        if (l.id == insertLink_ || l.id == selectedLink_) {
            dl->AddBezierCubic(a, c1, c2, b, IM_COL32(255, 255, 255, 90), thick * 3.0f);
            col = IM_COL32(255, 255, 255, 255);
            t = thick * 1.4f;
        }
        dl->AddBezierCubic(a, c1, c2, b, col, t);
    }
}

bool NodeEditor::drawValueBox(Node& n, int param, const ImRect& box, ImDrawList* dl, const char* label) {
    const ParamDesc& d = n.info().params[param];
    const float fs = ImGui::GetFontSize() * zoom_;
    bool changed = false;
    ImGui::PushID(param);
    ImGui::SetCursorScreenPos(box.Min);

    if (editing_.node == n.id && editing_.param == param) {
        // Text entry mode (click without dragging).
        float v = n.paramF(param);
        ImGui::SetNextItemWidth(box.GetWidth());
        ImGui::SetWindowFontScale(zoom_);
        if (editing_.frames++ == 0) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputFloat("##edit", &v, 0, 0, "%.3f", ImGuiInputTextFlags_AutoSelectAll)) {
            n.params[param] = std::clamp(v, d.min, d.max);
            changed = true;
        }
        const bool active = ImGui::IsItemActive();
        ImGui::SetWindowFontScale(1.0f);
        if (active) editing_.wasActive = true;
        else if (editing_.wasActive || editing_.frames > 3) editing_ = {};  // focus left the field: done
        ImGui::PopID();
        return changed;
    }

    static bool dragged = false;  // only one value box can be active at a time
    ImGui::InvisibleButton("##val", box.GetSize());
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActivated()) {
        dragged = false;
        activeNode_ = n.id;
        activeParam_ = param;
    }
    if (ImGui::IsItemDeactivated()) activeNode_ = 0, activeParam_ = -1;
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        const ImGuiIO& io = ImGui::GetIO();
        float speed = (d.max - d.min) / std::max(20.0f, box.GetWidth());
        if (io.KeyShift) speed *= 0.1f;
        float v = std::clamp(n.paramF(param) + io.MouseDelta.x * speed, d.min, d.max);
        if (v != n.paramF(param)) {
            n.params[param] = v;
            changed = true;
        }
        dragged = true;
    }
    if (ImGui::IsItemDeactivated() && !dragged) editing_ = EditState{n.id, param, 0, false};

    drawValueField(dl, box, label, n.paramF(param), d, fs, zoom_, ImGui::IsItemHovered() || ImGui::IsItemActive());
    ImGui::PopID();
    return changed;
}

// One param row on the node body. canInteract: this node is topmost under the mouse.
bool NodeEditor::drawParamRow(ImDrawList* dl, Node& n, int i, const ImRect& box, bool canInteract) {
    const ParamDesc& d = n.info().params[i];
    const float z = zoom_;
    const float fs = ImGui::GetFontSize() * z;
    const bool showText = fs >= 5.0f;
    const ImU32 textCol = IM_COL32(222, 222, 228, 255);
    const ImVec4 bclip(box.Min.x + 3 * z, box.Min.y, box.Max.x - 3 * z, box.Max.y);
    auto text = [&](float x, const char* s, ImU32 col) {
        if (showText) dl->AddText(ImGui::GetFont(), fs, ImVec2(x, box.GetCenter().y - fs * 0.5f), col, s, nullptr, 0.0f, &bclip);
    };
    const bool mineEditing = editing_.node == n.id && editing_.param == i;
    const bool mineActive = activeNode_ == n.id && activeParam_ == i;
    const bool mineMenu = enumNode_ == n.id && enumParam_ == i;
    bool changed = false;
    bool hovered = false;
    ImGui::PushID(2000 + i);

    auto button = [&]() {
        ImGui::SetCursorScreenPos(box.Min);
        bool clicked = ImGui::InvisibleButton("##row", box.GetSize());
        hovered = ImGui::IsItemHovered();
        return clicked;
    };

    switch (d.kind) {
        case ParamKind::Float:
            if (canInteract || mineEditing || mineActive) changed = drawValueBox(n, i, box, dl, d.name.c_str());
            else drawValueField(dl, box, d.name.c_str(), n.paramF(i), d, fs, z, false);
            break;
        case ParamKind::Path: {
            std::string path = n.paramS(i);
            std::string label = path.empty() ? "Choose image..." : pathToU8(u8ToPath(path).filename());
            if (canInteract && button()) {
                if (auto p = openFileDialog("Choose image", kImageFilter)) {
                    n.params[i] = *p;
                    changed = true;
                }
            }
            if (hovered && !path.empty()) ImGui::SetTooltip("%s", path.c_str());
            dl->AddRectFilled(box.Min, box.Max, hovered ? IM_COL32(70, 74, 88, 255) : IM_COL32(54, 56, 66, 255), 3 * z);
            text(box.Min.x + 6 * z, label.c_str(), textCol);
            break;
        }
        case ParamKind::Enum: {
            int v = n.paramI(i);
            const char* opt = (v >= 0 && v < int(d.options.size())) ? d.options[v].c_str() : "?";
            if (canInteract && button()) {
                enumNode_ = n.id;
                enumParam_ = i;
                ImGui::OpenPopup("##enum");
            }
            dl->AddRectFilled(box.Min, box.Max, hovered ? IM_COL32(70, 74, 88, 255) : IM_COL32(54, 56, 66, 255), 3 * z);
            text(box.Min.x + 6 * z, opt, textCol);
            float ax = box.Max.x - 10 * z, ay = box.GetCenter().y, as = 3.5f * z;
            dl->AddTriangleFilled(ImVec2(ax - as, ay - as * 0.6f), ImVec2(ax + as, ay - as * 0.6f), ImVec2(ax, ay + as * 0.8f), textCol);
            // The popup lives in this node's ID scope, so it is drawn here while open even if the
            // mouse has moved off the node.
            if (enumNode_ == n.id && enumParam_ == i) {
                if (ImGui::BeginPopup("##enum")) {
                    for (int k = 0; k < int(d.options.size()); ++k)
                        if (ImGui::Selectable(d.options[k].c_str(), k == v)) {
                            n.params[i] = k;
                            changed = true;
                        }
                    ImGui::EndPopup();
                } else {
                    enumNode_ = 0;
                    enumParam_ = -1;
                }
            }
            (void)mineMenu;
            break;
        }
        case ParamKind::Bool: {
            if (canInteract && button()) {
                n.params[i] = !n.paramB(i);
                changed = true;
            }
            const float sz = box.GetHeight() - 4 * z;
            ImVec2 c0(box.Min.x + 2 * z, box.GetCenter().y - sz * 0.5f), c1(c0.x + sz, c0.y + sz);
            dl->AddRectFilled(c0, c1, hovered ? IM_COL32(70, 74, 88, 255) : IM_COL32(54, 56, 66, 255), 3 * z);
            if (n.paramB(i))
                dl->AddRectFilled(ImVec2(c0.x + 3 * z, c0.y + 3 * z), ImVec2(c1.x - 3 * z, c1.y - 3 * z),
                                  IM_COL32(90, 130, 210, 255), 2 * z);
            text(c1.x + 6 * z, d.name.c_str(), textCol);
            break;
        }
        case ParamKind::Text: {
            if (mineEditing) {
                char buf[512];
                std::snprintf(buf, sizeof(buf), "%s", n.paramS(i).c_str());
                ImGui::SetCursorScreenPos(box.Min);
                ImGui::SetNextItemWidth(box.GetWidth());
                ImGui::SetWindowFontScale(z);
                if (editing_.frames++ == 0) ImGui::SetKeyboardFocusHere();
                if (ImGui::InputText("##text", buf, sizeof(buf))) {
                    n.params[i] = std::string(buf);
                    changed = true;
                }
                const bool active = ImGui::IsItemActive();
                ImGui::SetWindowFontScale(1.0f);
                if (active) editing_.wasActive = true;
                else if (editing_.wasActive || editing_.frames > 3) editing_ = {};
                break;
            }
            if (canInteract && button()) editing_ = EditState{n.id, i, 0, false};
            dl->AddRectFilled(box.Min, box.Max, hovered ? IM_COL32(34, 34, 40, 255) : IM_COL32(26, 26, 30, 255), 3 * z);
            text(box.Min.x + 6 * z, n.paramS(i).c_str(), IM_COL32(200, 230, 200, 255));
            if (hovered) ImGui::SetTooltip("%s - click to edit", d.name.c_str());
            break;
        }
        case ParamKind::Ramp: {
            // Gradient preview; stops are edited in the inspector.
            ColorRamp ramp = rampFromJson(n.params[i]);
            const int segs = 48;
            auto col = [](const float* c) { return ImGui::GetColorU32(ImVec4(c[0], c[1], c[2], 1.0f)); };
            for (int k = 0; k < segs; ++k) {
                float c0[4], c1[4];
                ramp.eval(float(k) / segs, c0);
                ramp.eval(float(k + 1) / segs, c1);
                float x0 = box.Min.x + box.GetWidth() * k / segs, x1 = box.Min.x + box.GetWidth() * (k + 1) / segs;
                dl->AddRectFilledMultiColor(ImVec2(x0, box.Min.y), ImVec2(x1, box.Max.y), col(c0), col(c1), col(c1), col(c0));
            }
            dl->AddRect(box.Min, box.Max, IM_COL32(20, 20, 24, 255));
            break;
        }
        case ParamKind::Curve: {
            dl->AddRectFilled(box.Min, box.Max, IM_COL32(26, 26, 30, 255), 3 * z);
            dl->AddLine(ImVec2(box.Min.x, box.Max.y), ImVec2(box.Max.x, box.Min.y), IM_COL32(60, 60, 68, 255));
            // Standard curves: r, g, b then master on top. Custom channels (hue curves, float curve)
            // come from the param's key list.
            std::vector<std::string> keys = {"r", "g", "b", "master"};
            std::vector<ImU32> cols = {IM_COL32(220, 80, 80, 200), IM_COL32(80, 200, 90, 200), IM_COL32(90, 130, 230, 200),
                                       IM_COL32(235, 235, 240, 255)};
            bool standard = true;
            if (!d.options.empty()) {
                keys.clear();
                cols.clear();
                standard = false;
                for (const auto& k : d.options)
                    if (k[0] != '@') keys.push_back(k.substr(0, k.find(':'))), cols.push_back(IM_COL32(235, 235, 240, 220));
            }
            const nlohmann::json& cj = n.params[i];
            for (int c = 0; c < int(keys.size()); ++c) {
                CurvePoints pts = curveFromJson(cj.is_object() && cj.contains(keys[c]) ? cj[keys[c]] : nlohmann::json());
                if (standard && c < 3 && isIdentityCurve(pts)) continue;
                ImVec2 prev;
                for (int k = 0; k <= 32; ++k) {
                    float x = k / 32.0f, y = evalCurve(pts, x);
                    ImVec2 p(box.Min.x + x * box.GetWidth(), box.Max.y - y * box.GetHeight());
                    if (k) dl->AddLine(prev, p, cols[c], 1.5f);
                    prev = p;
                }
            }
            break;
        }
        case ParamKind::Color: {
            float c[3];
            n.paramC(i, c);
            if (canInteract && button()) ImGui::OpenPopup("##color");
            const float sw = box.GetHeight() * 1.6f;
            ImRect swatch(ImVec2(box.Max.x - sw, box.Min.y), box.Max);
            dl->AddRectFilled(box.Min, box.Max, hovered ? IM_COL32(34, 34, 40, 255) : IM_COL32(26, 26, 30, 255), 3 * z);
            dl->AddRectFilled(swatch.Min, swatch.Max,
                              ImGui::GetColorU32(ImVec4(std::min(c[0], 1.0f), std::min(c[1], 1.0f), std::min(c[2], 1.0f), 1.0f)), 3 * z);
            text(box.Min.x + 6 * z, d.name.c_str(), textCol);
            if (ImGui::BeginPopup("##color")) {
                ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoAlpha;
                if (d.max > 1.0f) flags |= ImGuiColorEditFlags_HDR;  // lift/gain may go above 1
                if (ImGui::ColorPicker3("##picker", c, flags)) {
                    for (int k = 0; k < 3; ++k) c[k] = std::clamp(c[k], d.hardMin, d.hardMax);
                    n.params[i] = nlohmann::json::array({c[0], c[1], c[2]});
                    changed = true;
                }
                ImGui::EndPopup();
            }
            break;
        }
        case ParamKind::SavePath: {
            std::string path = n.paramS(i);
            std::string label = path.empty() ? "Save as..." : pathToU8(u8ToPath(path).filename());
            if (canInteract && button()) {
                if (auto p = saveFileDialog("File Output", "PNG image|*.png|JPEG image|*.jpg", "png")) {
                    n.params[i] = *p;
                    changed = true;
                }
            }
            if (hovered && !path.empty()) ImGui::SetTooltip("%s", path.c_str());
            dl->AddRectFilled(box.Min, box.Max, hovered ? IM_COL32(70, 74, 88, 255) : IM_COL32(54, 56, 66, 255), 3 * z);
            text(box.Min.x + 6 * z, label.c_str(), textCol);
            break;
        }
        case ParamKind::Int: break;
    }
    ImGui::PopID();
    return changed;
}

bool NodeEditor::drawNode(ImDrawList* dl, Graph& g, Node& n, int preview, Result& r) {
    const NodeInfo& info = n.info();
    const Layout L = layoutFor(n);
    const float z = zoom_;
    if (isReroute(n)) {
        // A dot: the wire's color (or amber when unconnected), outlined when selected.
        ImVec2 c((L.min.x + L.max.x) * 0.5f, (L.min.y + L.max.y) * 0.5f);
        ImU32 col = pinColor(PinType::Image);
        if (const Link* l = g.inputLink(n.id, 0))
            if (const Node* from = g.find(l->fromNode)) col = pinColor(from->info().outputs[l->fromPin].type);
        dl->AddCircleFilled(c, 6.0f * z + 1.0f, col);
        dl->AddCircle(c, 6.0f * z + 1.0f, selection_.count(n.id) ? IM_COL32(240, 196, 100, 255) : IM_COL32(20, 20, 24, 255), 0,
                      selection_.count(n.id) ? 2.0f : 1.0f);
        (void)r;
        return false;
    }
    const float fs = ImGui::GetFontSize() * z;
    const bool showText = fs >= 5.0f;
    const bool interactive = z >= 0.45f;
    const ImVec4 clip(L.min.x, L.min.y, L.max.x, L.max.y);
    const float round = 6.0f * z;
    const bool selected = selection_.count(n.id) > 0;
    bool changed = false;

    // body + title
    dl->AddRectFilled(ImVec2(L.min.x + 3, L.min.y + 4), ImVec2(L.max.x + 3, L.max.y + 4), IM_COL32(0, 0, 0, 70), round);
    dl->AddRectFilled(L.min, L.max, IM_COL32(40, 40, 46, 245), round);
    ImU32 titleCol = n.id == preview ? IM_COL32(196, 122, 38, 255) : categoryColor(info.category);
    if (n.muted) titleCol = IM_COL32(92, 58, 58, 255);
    dl->AddRectFilled(L.min, ImVec2(L.max.x, L.min.y + L.titleH), titleCol, round,
                      n.collapsed ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop);
    if (showText) {
        std::string title = n.label.empty() ? info.displayName : n.label;
        if (n.muted) title += "  (muted)";
        dl->AddText(ImGui::GetFont(), fs, ImVec2(L.min.x + (n.collapsed ? 14 : 8) * z, L.min.y + (L.titleH - fs) * 0.5f),
                    n.muted ? IM_COL32(200, 170, 170, 255) : IM_COL32(245, 245, 250, 255), title.c_str(), nullptr, 0.0f, &clip);
    }
    if (n.muted && !n.collapsed && !L.inPins.empty() && !L.outPins.empty()) {
        // Red pass-through line like Blender's muted nodes.
        dl->AddLine(L.inPins[0], L.outPins[0], IM_COL32(200, 70, 70, 200), std::max(1.5f, 2.0f * z));
    }
    dl->AddRect(L.min, L.max, selected ? IM_COL32(240, 196, 100, 255) : IM_COL32(18, 18, 22, 255), round, 0,
                selected ? 2.0f : 1.0f);

    // Only the topmost node under the mouse gets interactive widgets, so overlapping nodes behave.
    const bool ownsMouse = hitNode(g, ImGui::GetIO().MousePos) == n.id;
    ImGui::PushID(n.id);

    auto drawPin = [&](ImVec2 p, PinType t, bool hovered) {
        float pr = std::max(3.0f, kPinR * z) * (hovered ? 1.4f : 1.0f);
        if (t == PinType::Image) {
            dl->AddRectFilled(ImVec2(p.x - pr, p.y - pr), ImVec2(p.x + pr, p.y + pr), pinColor(t), 1.5f);
            dl->AddRect(ImVec2(p.x - pr, p.y - pr), ImVec2(p.x + pr, p.y + pr), IM_COL32(20, 20, 24, 255), 1.5f);
        } else {
            dl->AddCircleFilled(p, pr, pinColor(t));
            dl->AddCircle(p, pr, IM_COL32(20, 20, 24, 255));
        }
    };
    const ImU32 labelCol = IM_COL32(212, 212, 218, 255);
    if (n.collapsed) {
        for (int i = 0; i < int(info.outputs.size()); ++i) drawPin(L.outPins[i], info.outputs[i].type, false);
        for (int i = 0; i < int(info.inputs.size()); ++i) drawPin(L.inPins[i], info.inputs[i].type, false);
        ImGui::PopID();
        return false;
    }

    for (int i = 0; i < int(info.outputs.size()); ++i) {
        ImVec2 p = L.outPins[i];
        if (showText) {
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, info.outputs[i].name.c_str());
            dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x - 12 * z - ts.x, p.y - fs * 0.5f), labelCol,
                        info.outputs[i].name.c_str(), nullptr, 0.0f, &clip);
        }
        bool hov = hoverPin_.node == n.id && hoverPin_.output && hoverPin_.pin == i;
        drawPin(p, info.outputs[i].type, hov);
        if (n.id == preview && i == previewPin_ && info.outputs.size() > 1)
            dl->AddCircle(p, std::max(6.0f, 9.0f * z), IM_COL32(236, 150, 50, 255), 0, 2.0f);
    }

    for (int i = 0; i < int(info.params.size()); ++i) {
        const ImRect& box = L.paramBoxes[i];
        if (box.GetWidth() <= 0) continue;
        changed |= drawParamRow(dl, n, i, box, interactive && ownsMouse);
    }

    for (int i = 0; i < int(info.inputs.size()); ++i) {
        ImVec2 p = L.inPins[i];
        const bool linked = g.inputLink(n.id, i) != nullptr;
        const ImRect& box = L.valueBoxes[i];
        const int fp = info.inputs[i].fallbackParam;
        const char* name = info.inputs[i].name.c_str();
        if (!linked && box.GetWidth() > 0) {
            // Unconnected input with a value: show it as an editable field (label inside).
            const bool mine = (editing_.node == n.id && editing_.param == fp) ||
                              (activeNode_ == n.id && activeParam_ == fp);
            if (interactive && (ownsMouse || mine)) changed |= drawValueBox(n, fp, box, dl, name);
            else drawValueField(dl, box, name, n.paramF(fp), info.params[fp], fs, z, false);
        } else if (showText) {
            dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x + 12 * z, p.y - fs * 0.5f), labelCol, name, nullptr, 0.0f, &clip);
        }
        bool hov = hoverPin_.node == n.id && !hoverPin_.output && hoverPin_.pin == i;
        drawPin(p, info.inputs[i].type, hov);
    }

    ImGui::PopID();
    if (changed) r.evalChanged = r.docChanged = true;
    return changed;
}

// ---------------------------------------------------------------- gestures

void NodeEditor::updateInsertCandidate(const Graph& g) {
    insertLink_ = 0;
    if (selection_.size() != 1) return;
    const int id = *selection_.begin();
    const Node* n = g.find(id);
    if (!n || n->info().inputs.empty() || n->info().outputs.empty()) return;
    for (const Link& l : g.links())
        if (l.fromNode == id || l.toNode == id) return;  // only free-floating nodes get spliced in

    Layout L = layoutFor(*n);
    ImRect rect(L.min, L.max);
    rect.Expand(4.0f);
    const ImVec2 center = rect.GetCenter();
    float best = 1e30f;
    for (const Link& l : g.links()) {
        PinType fromT = g.find(l.fromNode)->info().outputs[l.fromPin].type;
        PinType toT = g.find(l.toNode)->info().inputs[l.toPin].type;
        int in = pickPin(g, *n, true, fromT);
        int out = pickPin(g, *n, false, toT);
        if (in < 0 || out < 0) continue;
        ImVec2 a, b, c1, c2;
        linkEnds(g, l, a, b);
        bezierPoints(a, b, zoom_, c1, c2);
        for (int s = 0; s <= 24; ++s) {
            ImVec2 p = ImBezierCubicCalc(a, c1, c2, b, s / 24.0f);
            if (!rect.Contains(p)) continue;
            float d = ImLengthSqr(p - center);
            if (d < best) {
                best = d;
                insertLink_ = l.id;
                insertIn_ = in;
                insertOut_ = out;
            }
        }
    }
}

void NodeEditor::finishLinkDrag(Graph& g, Result& r) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    auto connect = [&](PinRef a, PinRef b) {
        if (a.output == b.output) return;
        const PinRef& o = a.output ? a : b;
        const PinRef& i = a.output ? b : a;
        if (g.connect(o.node, o.pin, i.node, i.pin)) r.evalChanged = r.docChanged = true;
    };
    const PinRef target = hitPin(g, mouse);
    const int targetNode = hitNode(g, mouse);
    const Node* from = g.find(linkFrom_.node);
    if (!from) {
    } else if (target.valid() && target.node != linkFrom_.node) {
        connect(linkFrom_, target);
    } else if (targetNode && targetNode != linkFrom_.node) {
        // Dropped on a node body: use its best matching pin.
        const Node* tn = g.find(targetNode);
        PinType t = linkFrom_.output ? from->info().outputs[linkFrom_.pin].type : from->info().inputs[linkFrom_.pin].type;
        int pin = pickPin(g, *tn, linkFrom_.output, t);
        if (pin >= 0) connect(linkFrom_, PinRef{targetNode, pin, !linkFrom_.output});
    } else if (!targetNode && !linkDetached_) {
        // Dropped on empty canvas: offer the add menu; the new node gets connected.
        menuConnect_ = linkFrom_;
        menuPos_ = mouse;
        search_[0] = '\0';
        ImGui::OpenPopup("AddNode");
    }
    linkDetached_ = false;
    hoverPin_ = {};
}

bool NodeEditor::deleteSelection(Graph& g, int& preview, bool reconnect) {
    bool any = false;
    if (selectedLink_) {
        g.removeLink(selectedLink_);
        selectedLink_ = 0;
        any = true;
    }
    for (int id : selection_) {
        // Deleting a node in the middle of a chain joins its neighbours (one node at a time, so
        // deleting several consecutive nodes still leaves the chain connected).
        if (reconnect) g.bridgeNode(id);
        g.removeNode(id);
        if (preview == id) preview = 0;
        any = true;
    }
    selection_.clear();
    return any;
}

bool NodeEditor::duplicateSelection(Graph& g) {
    if (selection_.empty()) return false;
    std::map<int, int> remap;
    for (int id : selection_)
        if (Node* copy = g.duplicateNode(id, 30.0f, 30.0f)) remap[id] = copy->id;
    std::vector<Link> internal;
    for (const Link& l : g.links())
        if (remap.count(l.fromNode) && remap.count(l.toNode)) internal.push_back(l);
    for (const Link& l : internal) g.connect(remap[l.fromNode], l.fromPin, remap[l.toNode], l.toPin);
    selection_.clear();
    for (auto& [oldId, newId] : remap) selection_.insert(newId);
    selectedLink_ = 0;
    return true;
}

// ---------------------------------------------------------------- main entry

NodeEditor::Result NodeEditor::draw(Graph& g, int& selected, int& preview, int& previewPin) {
    Result r;
    ImGuiIO& io = ImGui::GetIO();
    previewPin_ = previewPin;
    origin_ = ImGui::GetCursorScreenPos();
    size_ = ImGui::GetContentRegionAvail();
    size_.x = std::max(size_.x, 1.0f);
    size_.y = std::max(size_.y, 1.0f);
    syncOrder(g);
    if (fitFrames_ > 0) {
        doFrame(g);
        --fitFrames_;
    }

    // Background item catches clicks that no node widget takes.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##canvas", size_,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool bgHovered = ImGui::IsItemHovered();
    const bool bgActivated = ImGui::IsItemActivated();
    const ImVec2 mouse = io.MousePos;
    const ImRect canvas(origin_, ImVec2(origin_.x + size_.x, origin_.y + size_.y));
    const bool mouseInCanvas =
        canvas.Contains(mouse) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(canvas.Min, canvas.Max, true);
    dl->AddRectFilled(canvas.Min, canvas.Max, IM_COL32(30, 30, 36, 255));
    drawGrid(dl);
    drawFrames(dl, g);
    drawLinks(dl, g);
    for (int id : std::vector<int>(order_))
        if (Node* n = g.find(id)) drawNode(dl, g, *n, preview, r);

    // Insert candidate is re-drawn on top so it stays visible under the dragged node.
    if (insertLink_) {
        for (const Link& l : g.links()) {
            if (l.id != insertLink_) continue;
            ImVec2 a, b, c1, c2;
            linkEnds(g, l, a, b);
            bezierPoints(a, b, zoom_, c1, c2);
            dl->AddBezierCubic(a, c1, c2, b, IM_COL32(255, 255, 255, 150), std::max(1.5f, 2.5f * zoom_));
        }
    }

    // Wire being dragged
    if (mode_ == Mode::DragLink) {
        if (const Node* from = g.find(linkFrom_.node)) {
            Layout L = layoutFor(*from);
            ImVec2 p = linkFrom_.output ? L.outPins[linkFrom_.pin] : L.inPins[linkFrom_.pin];
            PinType t = linkFrom_.output ? from->info().outputs[linkFrom_.pin].type : from->info().inputs[linkFrom_.pin].type;
            ImVec2 a = linkFrom_.output ? p : mouse, b = linkFrom_.output ? mouse : p, c1, c2;
            bezierPoints(a, b, zoom_, c1, c2);
            dl->AddBezierCubic(a, c1, c2, b, pinColor(t), std::max(1.5f, 2.5f * zoom_));
        }
    }
    if ((mode_ == Mode::Knife || mode_ == Mode::RerouteCut) && knife_.size() > 1) {
        ImU32 col = mode_ == Mode::Knife ? IM_COL32(230, 80, 80, 230) : IM_COL32(120, 200, 120, 230);
        dl->AddPolyline(knife_.data(), int(knife_.size()), col, 0, 2.0f);
    }
    if (mode_ == Mode::BoxSelect) {
        ImRect box(ImMin(pressPos_, mouse), ImMax(pressPos_, mouse));
        dl->AddRectFilled(box.Min, box.Max, IM_COL32(100, 140, 220, 40));
        dl->AddRect(box.Min, box.Max, IM_COL32(100, 140, 220, 200));
    }
    if (g.nodes().empty()) {
        const char* hint = "Right-click to add a node";
        ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(canvas.GetCenter().x - ts.x * 0.5f, canvas.GetCenter().y), IM_COL32(120, 120, 130, 255), hint);
    }
    dl->PopClipRect();

    // ---- grab (G / Shift+D): selection follows the mouse until a click
    if (mode_ == Mode::Grab) {
        ImVec2 delta((mouse.x - pressPos_.x) / zoom_, (mouse.y - pressPos_.y) / zoom_);
        for (auto& [id, start] : dragStart_)
            if (Node* n = g.find(id)) {
                n->x = std::round(start.x + delta.x);
                n->y = std::round(start.y + delta.y);
            }
        r.docChanged = true;
        updateInsertCandidate(g);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            finishDragNodes(g, r);
            mode_ = Mode::None;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            for (auto& [id, start] : dragStart_)
                if (Node* n = g.find(id)) n->x = start.x, n->y = start.y;
            insertLink_ = 0;
            mode_ = Mode::None;
        }
    }

    // ---- press
    if (mode_ == Mode::Grab) {
        // handled above
    } else if (bgActivated && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        pressPos_ = mouse;
        editing_ = {};
        const PinRef pin = hitPin(g, mouse);
        const int nid = pin.valid() ? 0 : hitNode(g, mouse);
        if (pin.valid()) {
            linkDetached_ = false;
            linkFrom_ = pin;
            if (!pin.output) {
                // Dragging off a connected input picks up its wire (drop elsewhere = disconnect).
                if (const Link* l = g.inputLink(pin.node, pin.pin)) {
                    linkFrom_ = PinRef{l->fromNode, l->fromPin, true};
                    g.removeLink(l->id);
                    linkDetached_ = true;
                    r.evalChanged = r.docChanged = true;
                }
            }
            mode_ = Mode::DragLink;
        } else if (nid) {
            order_.erase(std::find(order_.begin(), order_.end(), nid));
            order_.push_back(nid);
            selectedLink_ = 0;
            selectedFrame_ = 0;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && dynamic_cast<GroupNode*>(g.find(nid))) {
                r.enterGroup = nid;
                mode_ = Mode::None;
            } else if (io.KeyCtrl && io.KeyShift) {
                // Cycle through the node's outputs in the preview (Blender's Ctrl+Shift+click).
                const int nOut = std::max<int>(1, int(g.find(nid)->info().outputs.size()));
                if (preview == nid) {
                    previewPin = (previewPin + 1) % nOut;
                } else {
                    preview = nid;
                    previewPin = 0;
                }
                r.previewChanged = true;
                mode_ = Mode::None;
            } else if (io.KeyCtrl) {
                preview = (preview == nid) ? 0 : nid;
                previewPin = 0;
                r.previewChanged = true;
                mode_ = Mode::None;
            } else {
                if (io.KeyShift) {
                    if (!selection_.erase(nid)) selection_.insert(nid);
                } else if (!selection_.count(nid)) {
                    selection_ = {nid};
                }
                pressNode_ = nid;
                dragStart_.clear();
                for (int id : selection_) dragStart_[id] = ImVec2(g.find(id)->x, g.find(id)->y);
                mode_ = Mode::PressNode;
            }
        } else if (int fc = hitFrameCorner(g, mouse)) {
            Frame* f = g.findFrame(fc);
            selectedFrame_ = fc;
            selection_.clear();
            frameStart_[0] = f->x, frameStart_[1] = f->y, frameStart_[2] = f->w, frameStart_[3] = f->h;
            mode_ = Mode::ResizeFrame;
        } else if (int ft = hitFrameTitle(g, mouse)) {
            Frame* f = g.findFrame(ft);
            selectedFrame_ = ft;
            selection_.clear();
            selectedLink_ = 0;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                menuFrame_ = ft;
                std::snprintf(frameLabel_, sizeof(frameLabel_), "%s", f->label.c_str());
                ImGui::OpenPopup("FrameRename");
                mode_ = Mode::None;
            } else {
                // Nodes whose center lies inside the frame travel with it.
                frameStart_[0] = f->x, frameStart_[1] = f->y, frameStart_[2] = f->w, frameStart_[3] = f->h;
                frameNodes_.clear();
                for (const auto& [id, n] : g.nodes()) {
                    float cx = n->x + kNodeW * 0.5f, cy = n->y + nodeHeightGrid(*n) * 0.5f;
                    if (cx > f->x && cx < f->x + f->w && cy > f->y && cy < f->y + f->h) frameNodes_[id] = ImVec2(n->x, n->y);
                }
                mode_ = Mode::DragFrame;
            }
        } else if (int lid = hitLink(g, mouse)) {
            selectedLink_ = lid;
            selection_.clear();
            selectedFrame_ = 0;
            mode_ = Mode::None;
        } else {
            mode_ = io.KeyShift ? Mode::BoxSelect : Mode::Pan;
        }
    } else if (bgActivated && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        pressPos_ = mouse;
        mode_ = Mode::Pan;
    } else if (bgHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && mode_ == Mode::None && (io.KeyCtrl || io.KeyShift)) {
        knife_ = {mouse};
        mode_ = io.KeyCtrl ? Mode::Knife : Mode::RerouteCut;
    } else if (bgHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && mode_ == Mode::None) {
        menuPos_ = mouse;
        search_[0] = '\0';
        menuConnect_ = {};
        if (int nid = hitNode(g, mouse)) {
            menuNode_ = nid;
            if (!selection_.count(nid)) selection_ = {nid};
            ImGui::OpenPopup("NodeMenu");
        } else if (int ft = hitFrameTitle(g, mouse)) {
            menuFrame_ = ft;
            selectedFrame_ = ft;
            ImGui::OpenPopup("FrameMenu");
        } else {
            ImGui::OpenPopup("AddNode");
        }
    }

    // ---- ongoing gestures
    const bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    switch (mode_) {
        case Mode::Pan:
            pan_.x += io.MouseDelta.x;
            pan_.y += io.MouseDelta.y;
            if (!leftDown && !ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
                // A click (no drag) on empty canvas clears the selection.
                if (ImLengthSqr(mouse - pressPos_) < 9.0f && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                    selection_.clear();
                    selectedLink_ = 0;
                    selectedFrame_ = 0;
                }
                mode_ = Mode::None;
            }
            break;
        case Mode::BoxSelect:
            if (!leftDown) {
                ImRect box(ImMin(pressPos_, mouse), ImMax(pressPos_, mouse));
                selection_.clear();
                for (const auto& [id, n] : g.nodes()) {
                    Layout L = layoutFor(*n);
                    if (box.Overlaps(ImRect(L.min, L.max))) selection_.insert(id);
                }
                mode_ = Mode::None;
            }
            break;
        case Mode::PressNode:
            if (!leftDown) {
                if (!io.KeyShift) selection_ = {pressNode_};  // plain click on a node in a group selects just it
                mode_ = Mode::None;
            } else if (ImLengthSqr(mouse - pressPos_) > 9.0f) {
                mode_ = Mode::DragNodes;
                if (io.KeyAlt) {
                    // Alt+drag pulls the nodes out of their chain and closes the gap.
                    for (int id : selection_) g.bridgeNode(id);
                    r.evalChanged = r.docChanged = true;
                }
            }
            break;
        case Mode::DragNodes: {
            ImVec2 delta((mouse.x - pressPos_.x) / zoom_, (mouse.y - pressPos_.y) / zoom_);
            for (auto& [id, start] : dragStart_)
                if (Node* n = g.find(id)) {
                    n->x = std::round(start.x + delta.x);
                    n->y = std::round(start.y + delta.y);
                }
            r.docChanged = true;
            updateInsertCandidate(g);
            if (!leftDown) {
                finishDragNodes(g, r);
                mode_ = Mode::None;
            }
            break;
        }
        case Mode::Knife:
        case Mode::RerouteCut: {
            if (ImLengthSqr(mouse - knife_.back()) > 16.0f) knife_.push_back(mouse);
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
                // Every wire crossing the stroke is cut, or gets a reroute dot at the crossing.
                auto cross = [](ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImVec2& hit) {
                    float den = (b.x - a.x) * (d.y - c.y) - (b.y - a.y) * (d.x - c.x);
                    if (std::fabs(den) < 1e-6f) return false;
                    float t = ((c.x - a.x) * (d.y - c.y) - (c.y - a.y) * (d.x - c.x)) / den;
                    float u = ((c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x)) / den;
                    if (t < 0 || t > 1 || u < 0 || u > 1) return false;
                    hit = ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
                    return true;
                };
                std::vector<std::pair<Link, ImVec2>> hits;
                for (const Link& l : g.links()) {
                    ImVec2 a, b, c1, c2, prev, hit;
                    linkEnds(g, l, a, b);
                    bezierPoints(a, b, zoom_, c1, c2);
                    bool found = false;
                    prev = a;
                    for (int s = 1; s <= 24 && !found; ++s) {
                        ImVec2 p = ImBezierCubicCalc(a, c1, c2, b, s / 24.0f);
                        for (size_t k = 1; k < knife_.size() && !found; ++k) found = cross(prev, p, knife_[k - 1], knife_[k], hit);
                        prev = p;
                    }
                    if (found) hits.push_back({l, hit});
                }
                for (auto& [l, hit] : hits) {
                    g.removeLink(l.id);
                    if (mode_ == Mode::RerouteCut) {
                        ImVec2 gp = toGrid(hit);
                        if (Node* rr = g.addNode("util.reroute", std::round(gp.x - 10), std::round(gp.y - 10))) {
                            g.connect(l.fromNode, l.fromPin, rr->id, 0);
                            g.connect(rr->id, 0, l.toNode, l.toPin);
                        }
                    }
                }
                if (!hits.empty()) r.evalChanged = r.docChanged = true;
                knife_.clear();
                mode_ = Mode::None;
            }
            break;
        }
        case Mode::Grab: break;
        case Mode::DragLink:
            hoverPin_ = hitPin(g, mouse);
            if (!leftDown) {
                finishLinkDrag(g, r);
                mode_ = Mode::None;
            }
            break;
        case Mode::DragFrame:
        case Mode::ResizeFrame: {
            Frame* f = g.findFrame(selectedFrame_);
            if (!f) {
                mode_ = Mode::None;
                break;
            }
            ImVec2 d((mouse.x - pressPos_.x) / zoom_, (mouse.y - pressPos_.y) / zoom_);
            if (mode_ == Mode::DragFrame) {
                f->x = std::round(frameStart_[0] + d.x);
                f->y = std::round(frameStart_[1] + d.y);
                for (auto& [id, start] : frameNodes_)
                    if (Node* n = g.find(id)) {
                        n->x = std::round(start.x + d.x);
                        n->y = std::round(start.y + d.y);
                    }
            } else {
                f->w = std::max(120.0f, std::round(frameStart_[2] + d.x));
                f->h = std::max(80.0f, std::round(frameStart_[3] + d.y));
            }
            r.docChanged = true;
            if (!leftDown) mode_ = Mode::None;
            break;
        }
        case Mode::None: break;
    }

    // ---- zoom around the cursor
    if (mouseInCanvas && io.MouseWheel != 0.0f && !ImGui::IsAnyItemActive()) {
        ImVec2 gp = toGrid(mouse);
        zoom_ = std::clamp(zoom_ * std::pow(1.15f, io.MouseWheel), kMinZoom, kMaxZoom);
        pan_ = ImVec2(mouse.x - origin_.x - gp.x * zoom_, mouse.y - origin_.y - gp.y * zoom_);
    }

    // ---- keyboard
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && !io.WantTextInput) {
        const bool noMods = !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace) ||
            (ImGui::IsKeyPressed(ImGuiKey_X) && noMods)) {
            if (selectedFrame_) {
                g.removeFrame(selectedFrame_);  // the frame only; its nodes stay
                selectedFrame_ = 0;
                r.docChanged = true;
            } else if (deleteSelection(g, preview, !io.KeyAlt)) {  // Alt: delete without reconnecting
                r.evalChanged = r.docChanged = true;
            }
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copySelection(g);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V) && paste(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyPressed(ImGuiKey_M) && noMods && toggleMute(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyPressed(ImGuiKey_H) && noMods && toggleCollapse(g)) r.docChanged = true;
        if (ImGui::IsKeyPressed(ImGuiKey_G) && noMods && !selection_.empty()) beginGrab(g);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_D) && duplicateSelection(g)) {
            beginGrab(g);
            r.evalChanged = r.docChanged = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F) && noMods && makeLinks(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyPressed(ImGuiKey_L) && !io.KeyCtrl && !io.KeyAlt) selectLinked(g, io.KeyShift);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) fitFrames_ = 1;
        if ((ImGui::IsKeyPressed(ImGuiKey_Period) || ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal)) && noMods) frameSelected(g);
        if (ImGui::IsKeyPressed(ImGuiKey_F2) && selection_.size() == 1) {
            renameNode_ = *selection_.begin();
            std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", g.find(renameNode_)->label.c_str());
            ImGui::OpenPopup("NodeRename");
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G) && groupSelection(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_G) && ungroupSelection(g))
            r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_J) && frameSelection(g)) r.docChanged = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_P) && moveSelectionToFrame(g, 0)) r.docChanged = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Tab) && !io.KeyCtrl) {
            if (int gid = selectedGroup(g)) r.enterGroup = gid;
            else r.exitGroup = true;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D) && duplicateSelection(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A))
            for (const auto& [id, n] : g.nodes()) selection_.insert(id);
    }

    drawAddMenu(g, r);
    drawNodeMenu(g, preview, r);
    drawFrameMenu(g, r);
    drawRenamePopup(g, r);

    // Node widgets moved the layout cursor around; leave it at a valid spot covering the canvas.
    ImGui::SetCursorScreenPos(origin_);
    ImGui::Dummy(size_);

    selected = selection_.size() == 1 ? *selection_.begin() : 0;
    return r;
}

// ---------------------------------------------------------------- menus

void NodeEditor::drawAddMenu(Graph& g, Result& r) {
    if (!ImGui::BeginPopup("AddNode")) {
        menuConnect_ = {};
        return;
    }
    // When a wire was dropped here, only offer nodes that can accept / provide it.
    const Node* src = g.find(menuConnect_.node);
    if (!src) menuConnect_ = {};
    PinType wireType = PinType::Image;
    if (src) wireType = menuConnect_.output ? src->info().outputs[menuConnect_.pin].type
                                            : src->info().inputs[menuConnect_.pin].type;
    auto compatible = [&](const NodeInfo& inf) {
        if (!src) return true;
        const auto& pins = menuConnect_.output ? inf.inputs : inf.outputs;
        return std::any_of(pins.begin(), pins.end(), [&](const PinDesc& p) {
            return menuConnect_.output ? canConvert(wireType, p.type) : canConvert(p.type, wireType);
        });
    };

    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(260);
    // Typing goes straight here (focused when the menu opens); empty search shows categories.
    if (ImGui::InputTextWithHint("##search", "Search nodes...", search_, sizeof(search_))) searchSel_ = 0;

    const auto& reg = NodeRegistry::instance();
    std::string chosen;
    if (search_[0]) {
        // Name matches first, then category matches.
        std::vector<std::string> results;
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& type : reg.types()) {
                const NodeInfo* inf = reg.find(type);
                if (inf->hidden || !compatible(*inf)) continue;
                bool nameHit = containsNoCase(inf->displayName, search_);
                if (pass == 0 ? nameHit : (!nameHit && containsNoCase(inf->category, search_))) results.push_back(type);
            }
        const int n = int(results.size());
        bool moved = false;
        if (n > 0) {
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) searchSel_ = (searchSel_ + 1) % n, moved = true;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) searchSel_ = (searchSel_ + n - 1) % n, moved = true;
        }
        searchSel_ = n ? std::clamp(searchSel_, 0, n - 1) : 0;
        if (n == 0) ImGui::TextDisabled("No matching nodes");
        const float rowH = ImGui::GetTextLineHeightWithSpacing();
        ImGui::BeginChild("##results", ImVec2(300, std::min(n, 14) * rowH + 6), ImGuiChildFlags_None);
        for (int i = 0; i < n; ++i) {
            const NodeInfo* inf = reg.find(results[i]);
            ImGui::PushID(i);
            if (ImGui::Selectable(inf->displayName.c_str(), i == searchSel_)) chosen = results[i];
            if (i == searchSel_ && moved) ImGui::SetScrollHereY();
            ImGui::SameLine(190);
            ImGui::TextDisabled("%s", inf->category.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (chosen.empty() && n > 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)))
            chosen = results[searchSel_];
    } else {
        std::vector<std::string> cats;
        for (const auto& type : reg.types()) {
            const NodeInfo* inf = reg.find(type);
            if (!inf->hidden && compatible(*inf) && std::find(cats.begin(), cats.end(), inf->category) == cats.end())
                cats.push_back(inf->category);
        }
        for (const auto& c : cats) {
            if (ImGui::BeginMenu(c.c_str())) {
                for (const auto& type : reg.types()) {
                    const NodeInfo* inf = reg.find(type);
                    if (inf->category == c && !inf->hidden && compatible(*inf) && ImGui::MenuItem(inf->displayName.c_str()))
                        chosen = type;
                }
                ImGui::EndMenu();
            }
        }
        if (!src) {
            ImGui::Separator();
            if (ImGui::MenuItem("Frame", "Ctrl+J")) {
                ImVec2 gp = toGrid(menuPos_);
                g.addFrame(std::round(gp.x), std::round(gp.y), 360, 240);
                r.docChanged = true;
                ImGui::CloseCurrentPopup();
            }
        }
    }

    if (!chosen.empty()) {
        if (Node* n = g.addNode(chosen)) {
            placeAtScreen(*n, menuPos_);
            if (src) {
                int pin = pickPin(g, *n, menuConnect_.output, wireType);
                if (pin >= 0) {
                    if (menuConnect_.output) g.connect(menuConnect_.node, menuConnect_.pin, n->id, pin);
                    else g.connect(n->id, pin, menuConnect_.node, menuConnect_.pin);
                }
            }
            select(n->id);
            r.evalChanged = r.docChanged = true;
        }
        menuConnect_ = {};
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void NodeEditor::drawNodeMenu(Graph& g, int& preview, Result& r) {
    if (!ImGui::BeginPopup("NodeMenu")) return;
    bool openRename = false;
    if (ImGui::MenuItem("Duplicate", "Ctrl+D") && duplicateSelection(g)) r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem(preview == menuNode_ ? "Stop Previewing" : "Preview", "Ctrl+Click")) {
        preview = (preview == menuNode_) ? 0 : menuNode_;
        r.previewChanged = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Copy", "Ctrl+C")) copySelection(g);
    Node* mn = g.find(menuNode_);
    if (ImGui::MenuItem("Mute", "M", mn && mn->muted) && toggleMute(g)) r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem("Collapse", "H", mn && mn->collapsed) && toggleCollapse(g)) r.docChanged = true;
    if (ImGui::MenuItem("Rename...", "F2") && mn) {
        renameNode_ = menuNode_;
        std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", mn->label.c_str());
        openRename = true;
    }
    if (ImGui::MenuItem("Make Links", "F", false, selection_.size() > 1) && makeLinks(g)) r.evalChanged = r.docChanged = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Group", "Ctrl+G") && groupSelection(g)) r.evalChanged = r.docChanged = true;
    const bool isGroup = dynamic_cast<GroupNode*>(g.find(menuNode_)) != nullptr;
    if (ImGui::MenuItem("Ungroup", "Ctrl+Alt+G", false, isGroup) && ungroupSelection(g)) r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem("Edit Group", "Tab", false, isGroup)) r.enterGroup = menuNode_;
    if (ImGui::MenuItem("Frame Selection", "Ctrl+J") && frameSelection(g)) r.docChanged = true;
    if (ImGui::BeginMenu("Move to Frame", !g.frames().empty() || (mn && frameOf(g, *mn)))) {
        const int current = mn ? frameOf(g, *mn) : 0;
        for (const Frame& f : g.frames()) {
            ImGui::PushID(f.id);
            if (ImGui::MenuItem(f.label.c_str(), nullptr, f.id == current) && moveSelectionToFrame(g, f.id))
                r.docChanged = true;
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Remove from Frame", "Alt+P", false, current != 0) && moveSelectionToFrame(g, 0))
            r.docChanged = true;
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete (reconnect)", "Del / X") && deleteSelection(g, preview, true)) r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem("Delete", "Alt+Del") && deleteSelection(g, preview, false)) r.evalChanged = r.docChanged = true;
    ImGui::EndPopup();
    if (openRename) ImGui::OpenPopup("NodeRename");
}

// ---------------------------------------------------------------- frames

void NodeEditor::drawFrames(ImDrawList* dl, const Graph& g) const {
    const float z = zoom_;
    const float fs = ImGui::GetFontSize() * z;
    for (const Frame& f : g.frames()) {
        ImVec2 a = toScreen(ImVec2(f.x, f.y)), b = toScreen(ImVec2(f.x + f.w, f.y + f.h));
        ImU32 body = ImGui::GetColorU32(ImVec4(f.color[0], f.color[1], f.color[2], 0.35f));
        ImU32 title = ImGui::GetColorU32(ImVec4(f.color[0] * 1.2f, f.color[1] * 1.2f, f.color[2] * 1.2f, 0.85f));
        dl->AddRectFilled(a, b, body, 6 * z);
        dl->AddRectFilled(a, ImVec2(b.x, a.y + kTitleH * z), title, 6 * z, ImDrawFlags_RoundCornersTop);
        const bool sel = f.id == selectedFrame_;
        dl->AddRect(a, b, sel ? IM_COL32(240, 196, 100, 255) : IM_COL32(0, 0, 0, 80), 6 * z, 0, sel ? 2.0f : 1.0f);
        if (fs >= 5.0f) {
            ImVec4 clip(a.x, a.y, b.x, b.y);
            dl->AddText(ImGui::GetFont(), fs * 1.1f, ImVec2(a.x + 8 * z, a.y + (kTitleH * z - fs * 1.1f) * 0.5f),
                        IM_COL32(245, 245, 250, 255), f.label.c_str(), nullptr, 0.0f, &clip);
        }
        // resize grip
        float gs = 12 * z;
        dl->AddTriangleFilled(ImVec2(b.x - gs, b.y - 2), ImVec2(b.x - 2, b.y - gs), ImVec2(b.x - 2, b.y - 2),
                              IM_COL32(255, 255, 255, 60));
    }
}

int NodeEditor::hitFrameTitle(const Graph& g, ImVec2 p) const {
    for (auto it = g.frames().rbegin(); it != g.frames().rend(); ++it) {
        ImVec2 a = toScreen(ImVec2(it->x, it->y)), b = toScreen(ImVec2(it->x + it->w, it->y + kTitleH));
        if (ImRect(a, b).Contains(p)) return it->id;
    }
    return 0;
}

int NodeEditor::hitFrameCorner(const Graph& g, ImVec2 p) const {
    for (auto it = g.frames().rbegin(); it != g.frames().rend(); ++it) {
        ImVec2 b = toScreen(ImVec2(it->x + it->w, it->y + it->h));
        float gs = std::max(10.0f, 14 * zoom_);
        if (ImRect(ImVec2(b.x - gs, b.y - gs), b).Contains(p)) return it->id;
    }
    return 0;
}

void NodeEditor::drawFrameMenu(Graph& g, Result& r) {
    if (ImGui::BeginPopup("FrameMenu")) {
        Frame* f = g.findFrame(menuFrame_);
        if (!f) {
            ImGui::CloseCurrentPopup();
        } else {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", f->label.c_str());
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText("Label", buf, sizeof(buf))) {
                f->label = buf;
                r.docChanged = true;
            }
            static const float presets[][3] = {{0.30f, 0.34f, 0.42f}, {0.45f, 0.22f, 0.22f}, {0.22f, 0.40f, 0.24f},
                                               {0.22f, 0.30f, 0.50f}, {0.46f, 0.40f, 0.18f}, {0.38f, 0.24f, 0.46f}};
            for (int k = 0; k < 6; ++k) {
                if (k) ImGui::SameLine();
                ImGui::PushID(k);
                if (ImGui::ColorButton("##preset", ImVec4(presets[k][0], presets[k][1], presets[k][2], 1.0f))) {
                    std::copy(presets[k], presets[k] + 3, f->color);
                    r.docChanged = true;
                }
                ImGui::PopID();
            }
            if (ImGui::ColorEdit3("Color", f->color, ImGuiColorEditFlags_NoInputs)) r.docChanged = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Move Selected Nodes Here", nullptr, false, !selection_.empty()) &&
                moveSelectionToFrame(g, menuFrame_))
                r.docChanged = true;
            if (ImGui::MenuItem("Fit to Contents")) {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const auto& [id, n] : g.nodes()) {
                    float cx = n->x + kNodeW * 0.5f, cy = n->y + nodeHeightGrid(*n) * 0.5f;
                    if (cx < f->x || cx > f->x + f->w || cy < f->y || cy > f->y + f->h) continue;
                    x0 = std::min(x0, n->x), y0 = std::min(y0, n->y);
                    x1 = std::max(x1, n->x + kNodeW), y1 = std::max(y1, n->y + nodeHeightGrid(*n));
                }
                if (x1 > x0) {
                    f->x = x0 - 24, f->y = y0 - kTitleH - 24, f->w = x1 - x0 + 48, f->h = y1 - y0 + kTitleH + 48;
                    r.docChanged = true;
                }
            }
            if (ImGui::MenuItem("Delete Frame", "Del")) {
                g.removeFrame(menuFrame_);
                selectedFrame_ = 0;
                r.docChanged = true;
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("FrameRename")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(220);
        bool done = ImGui::InputText("##label", frameLabel_, sizeof(frameLabel_), ImGuiInputTextFlags_EnterReturnsTrue);
        if (Frame* f = g.findFrame(menuFrame_); f && f->label != frameLabel_) {
            f->label = frameLabel_;
            r.docChanged = true;
        }
        if (done) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

bool NodeEditor::frameSelection(Graph& g) {
    if (selection_.empty()) {
        ImVec2 gp = toGrid(ImGui::GetIO().MousePos);
        selectedFrame_ = g.addFrame(std::round(gp.x), std::round(gp.y), 360, 240)->id;
        return true;
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int id : selection_)
        if (const Node* n = g.find(id)) {
            x0 = std::min(x0, n->x), y0 = std::min(y0, n->y);
            x1 = std::max(x1, n->x + kNodeW), y1 = std::max(y1, n->y + nodeHeightGrid(*n));
        }
    Frame* f = g.addFrame(x0 - 24, y0 - kTitleH - 24, x1 - x0 + 48, y1 - y0 + kTitleH + 48);
    selectedFrame_ = f->id;
    return true;
}

namespace {
float nodeWidthGrid(const Node& n) { return isReroute(n) ? kRerouteSize : kNodeW; }
bool frameHolds(const Frame& f, const Node& n) {
    float cx = n.x + nodeWidthGrid(n) * 0.5f, cy = n.y + nodeHeightGrid(n) * 0.5f;
    return cx > f.x && cx < f.x + f.w && cy > f.y && cy < f.y + f.h;
}
}  // namespace

int NodeEditor::frameOf(const Graph& g, const Node& n) const {
    int best = 0;
    float bestArea = 0;
    for (const Frame& f : g.frames())
        if (frameHolds(f, n) && (!best || f.w * f.h < bestArea)) best = f.id, bestArea = f.w * f.h;
    return best;
}

bool NodeEditor::moveSelectionToFrame(Graph& g, int frameId) {
    std::vector<Node*> moving;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int id : selection_)
        if (Node* n = g.find(id); n && (frameId ? frameOf(g, *n) != frameId : frameOf(g, *n) != 0)) {
            moving.push_back(n);
            x0 = std::min(x0, n->x), y0 = std::min(y0, n->y);
            x1 = std::max(x1, n->x + nodeWidthGrid(*n)), y1 = std::max(y1, n->y + nodeHeightGrid(*n));
        }
    if (moving.empty()) return false;
    constexpr float kMargin = 24.0f, kGap = 40.0f;

    // Target spot for the selection's top-left corner, keeping the nodes' layout relative to each other.
    float tx, ty;
    if (frameId) {
        Frame* f = g.findFrame(frameId);
        if (!f) return false;
        // Next to whatever the frame already holds, or at its top-left when it's empty.
        float mx1 = -1e9f, my0 = 1e9f;
        for (const auto& [id, n] : g.nodes())
            if (!selection_.count(id) && frameOf(g, *n) == frameId)
                mx1 = std::max(mx1, n->x + nodeWidthGrid(*n)), my0 = std::min(my0, n->y);
        if (mx1 > -1e9f) tx = mx1 + kGap, ty = my0;
        else tx = f->x + kMargin, ty = f->y + kTitleH + kMargin;
        // Grow the frame so the nodes' centres (and bodies) end up inside it.
        f->w = std::max(f->w, tx + (x1 - x0) + kMargin - f->x);
        f->h = std::max(f->h, ty + (y1 - y0) + kMargin - f->y);
    } else {
        // Out of the frame, just to the right of the outermost frame the nodes were in.
        float fx1 = -1e9f;
        for (const Frame& f : g.frames())
            for (Node* n : moving)
                if (frameHolds(f, *n)) fx1 = std::max(fx1, f.x + f.w);
        tx = fx1 + kGap, ty = y0;
    }
    for (Node* n : moving) n->x = std::round(n->x - x0 + tx), n->y = std::round(n->y - y0 + ty);
    return true;
}

// ---------------------------------------------------------------- groups

int NodeEditor::selectedGroup(const Graph& g) const {
    if (selection_.size() != 1) return 0;
    return dynamic_cast<GroupNode*>(g.find(*selection_.begin())) ? *selection_.begin() : 0;
}

bool NodeEditor::groupSelection(Graph& g) {
    if (selection_.empty()) return false;
    int gid = groupNodes(g, selection_);
    if (!gid) return false;
    select(gid);
    return true;
}

bool NodeEditor::ungroupSelection(Graph& g) {
    std::set<int> restored;
    bool any = false;
    for (int id : std::set<int>(selection_)) {
        if (!dynamic_cast<GroupNode*>(g.find(id))) continue;
        for (int n : ungroupNode(g, id)) restored.insert(n);
        any = true;
    }
    if (any) selection_ = restored;
    return any;
}

// ---------------------------------------------------------------- Blender-style conveniences

void NodeEditor::beginGrab(Graph& g) {
    dragStart_.clear();
    for (int id : selection_)
        if (Node* n = g.find(id)) dragStart_[id] = ImVec2(n->x, n->y);
    pressPos_ = ImGui::GetIO().MousePos;
    mode_ = Mode::Grab;
}

void NodeEditor::finishDragNodes(Graph& g, Result& r) {
    if (insertLink_) {
        // Splice the dragged node into the wire it was dropped on.
        Link l = *std::find_if(g.links().begin(), g.links().end(), [&](const Link& k) { return k.id == insertLink_; });
        const int id = *selection_.begin();
        g.removeLink(l.id);
        g.connect(l.fromNode, l.fromPin, id, insertIn_);
        g.connect(id, insertOut_, l.toNode, l.toPin);
        r.evalChanged = r.docChanged = true;

        // Auto-offset: if the spliced node overlaps what comes after it, push the downstream
        // nodes right to make room (Blender does the same).
        Node* n = g.find(id);
        Node* next = g.find(l.toNode);
        const float gap = 40.0f;
        if (n && next && next->x < n->x + kNodeW + gap) {
            const float shift = n->x + kNodeW + gap - next->x;
            std::set<int> down{l.toNode};
            std::vector<int> stack{l.toNode};
            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                for (const Link& k : g.links())
                    if (k.fromNode == cur && k.toNode != id && down.insert(k.toNode).second) stack.push_back(k.toNode);
            }
            for (int d : down)
                if (Node* dn = g.find(d)) dn->x += shift;
        }
    }
    insertLink_ = 0;
}

void NodeEditor::copySelection(const Graph& g) {
    if (selection_.empty()) return;
    nlohmann::json all = g.toJson();
    nlohmann::json clip = {{"nodelabClipboard", 1}, {"nodes", nlohmann::json::array()}, {"links", nlohmann::json::array()}};
    for (const auto& n : all["nodes"])
        if (selection_.count(n["id"].get<int>())) clip["nodes"].push_back(n);
    for (const auto& l : all["links"])
        if (selection_.count(l["from"][0].get<int>()) && selection_.count(l["to"][0].get<int>())) clip["links"].push_back(l);
    ImGui::SetClipboardText(clip.dump().c_str());
}

bool NodeEditor::paste(Graph& g) {
    const char* text = ImGui::GetClipboardText();
    if (!text) return false;
    nlohmann::json clip = nlohmann::json::parse(text, nullptr, false);
    if (clip.is_discarded() || !clip.contains("nodelabClipboard")) return false;
    // Rebuild in a scratch graph, then clone into this one around the mouse.
    Graph tmp;
    try {
        tmp.fromJson({{"nextId", 1}, {"nodes", clip["nodes"]}, {"links", clip["links"]}});
    } catch (const std::exception&) {
        return false;
    }
    if (tmp.nodes().empty()) return false;
    float x0 = 1e9f, y0 = 1e9f;
    for (const auto& [id, n] : tmp.nodes()) x0 = std::min(x0, n->x), y0 = std::min(y0, n->y);
    ImVec2 at = toGrid(ImGui::GetIO().MousePos);
    std::map<int, int> remap;
    for (const auto& [id, n] : tmp.nodes())
        if (Node* c = g.cloneNode(*n, std::round(n->x - x0 + at.x), std::round(n->y - y0 + at.y))) remap[id] = c->id;
    for (const Link& l : tmp.links())
        if (remap.count(l.fromNode) && remap.count(l.toNode)) g.connect(remap[l.fromNode], l.fromPin, remap[l.toNode], l.toPin);
    selection_.clear();
    for (auto& [a, b] : remap) selection_.insert(b);
    selectedLink_ = 0;
    return true;
}

bool NodeEditor::toggleMute(Graph& g) {
    bool any = false, allMuted = true;
    for (int id : selection_)
        if (Node* n = g.find(id); n && n->info().outputs.size() && !n->muted) allMuted = false;
    for (int id : selection_)
        if (Node* n = g.find(id); n && !n->info().outputs.empty()) {
            n->muted = !allMuted;
            any = true;
        }
    return any;
}

bool NodeEditor::toggleCollapse(Graph& g) {
    bool any = false, allCollapsed = true;
    for (int id : selection_)
        if (Node* n = g.find(id); n && !n->collapsed) allCollapsed = false;
    for (int id : selection_)
        if (Node* n = g.find(id); n && !isReroute(*n)) {
            n->collapsed = !allCollapsed;
            any = true;
        }
    return any;
}

bool NodeEditor::makeLinks(Graph& g) {
    // Left-to-right, connect each node's first output to the next node's best free input.
    std::vector<Node*> nodes;
    for (int id : selection_)
        if (Node* n = g.find(id)) nodes.push_back(n);
    std::sort(nodes.begin(), nodes.end(), [](Node* a, Node* b) { return a->x < b->x; });
    bool any = false;
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        const Node* a = nodes[i];
        const Node* b = nodes[i + 1];
        for (int o = 0; o < int(a->info().outputs.size()); ++o) {
            int in = pickPin(g, *b, true, a->info().outputs[o].type);
            if (in >= 0 && !g.inputLink(b->id, in) && g.connect(a->id, o, b->id, in)) {
                any = true;
                break;
            }
        }
    }
    return any;
}

void NodeEditor::selectLinked(const Graph& g, bool downstream) {
    std::vector<int> stack(selection_.begin(), selection_.end());
    while (!stack.empty()) {
        int cur = stack.back();
        stack.pop_back();
        for (const Link& l : g.links()) {
            int next = downstream ? (l.fromNode == cur ? l.toNode : 0) : (l.toNode == cur ? l.fromNode : 0);
            if (next && selection_.insert(next).second) stack.push_back(next);
        }
    }
}

void NodeEditor::frameSelected(const Graph& g) {
    if (selection_.empty()) {
        fitFrames_ = 1;
        return;
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int id : selection_)
        if (const Node* n = g.find(id)) {
            x0 = std::min(x0, n->x), y0 = std::min(y0, n->y);
            x1 = std::max(x1, n->x + kNodeW), y1 = std::max(y1, n->y + nodeHeightGrid(*n));
        }
    zoom_ = std::clamp(std::min((size_.x - 80) / (x1 - x0), (size_.y - 80) / (y1 - y0)), kMinZoom, 1.5f);
    pan_ = ImVec2(size_.x * 0.5f - (x0 + x1) * 0.5f * zoom_, size_.y * 0.5f - (y0 + y1) * 0.5f * zoom_);
}

void NodeEditor::drawRenamePopup(Graph& g, Result& r) {
    if (!ImGui::BeginPopup("NodeRename")) return;
    Node* n = g.find(renameNode_);
    if (!n) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextDisabled("Label for %s (empty = default)", n->info().displayName.c_str());
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(240);
    bool done = ImGui::InputText("##label", renameBuf_, sizeof(renameBuf_), ImGuiInputTextFlags_EnterReturnsTrue);
    if (n->label != renameBuf_) {
        n->label = renameBuf_;
        r.docChanged = true;
    }
    if (done) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
