#include "ui/NodeEditor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include <imgui_internal.h>

#include "graph/NodeRegistry.h"
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
    if (cat == "Math / Mix") return IM_COL32(104, 74, 136, 255);
    return IM_COL32(80, 80, 92, 255);
}

float nodeHeightGrid(const Node& n) {
    const NodeInfo& info = n.info();
    int rows = int(info.inputs.size() + info.outputs.size());
    for (const auto& p : info.params)
        if (p.kind == ParamKind::Path) ++rows;
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
    L.max = ImVec2(L.min.x + kNodeW * z, L.min.y + nodeHeightGrid(n) * z);
    L.titleH = kTitleH * z;
    float y = L.min.y + (kTitleH + kPad) * z;
    const float row = kRowH * z;
    for (size_t i = 0; i < info.outputs.size(); ++i, y += row) L.outPins.emplace_back(L.max.x, y + row * 0.5f);
    L.pathBoxes.resize(info.params.size());
    for (size_t i = 0; i < info.params.size(); ++i) {
        if (info.params[i].kind != ParamKind::Path) continue;
        L.pathBoxes[i] = ImRect(L.min.x + 8 * z, y + 2 * z, L.max.x - 8 * z, y + row - 2 * z);
        y += row;
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

void NodeEditor::syncOrder(const Graph& g) {
    std::erase_if(order_, [&](int id) { return !g.find(id); });
    for (const auto& [id, n] : g.nodes())
        if (std::find(order_.begin(), order_.end(), id) == order_.end()) order_.push_back(id);
    for (auto it = selection_.begin(); it != selection_.end();) it = g.find(*it) ? std::next(it) : selection_.erase(it);
    if (selectedLink_ && std::none_of(g.links().begin(), g.links().end(),
                                      [&](const Link& l) { return l.id == selectedLink_; }))
        selectedLink_ = 0;
}

void NodeEditor::doFrame(const Graph& g) {
    fitPending_ = false;
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
    insertLink_ = 0;
    hoverPin_ = {};
    if (frame) fitPending_ = true;
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
    fitPending_ = false;
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
    if (ImGui::IsItemActivated()) dragged = false;
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

bool NodeEditor::drawNode(ImDrawList* dl, Graph& g, Node& n, int preview, Result& r) {
    const NodeInfo& info = n.info();
    const Layout L = layoutFor(n);
    const float z = zoom_;
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
    dl->AddRectFilled(L.min, ImVec2(L.max.x, L.min.y + L.titleH), titleCol, round, ImDrawFlags_RoundCornersTop);
    if (showText)
        dl->AddText(ImGui::GetFont(), fs, ImVec2(L.min.x + 8 * z, L.min.y + (L.titleH - fs) * 0.5f),
                    IM_COL32(245, 245, 250, 255), info.displayName.c_str(), nullptr, 0.0f, &clip);
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

    for (int i = 0; i < int(info.outputs.size()); ++i) {
        ImVec2 p = L.outPins[i];
        if (showText) {
            ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, info.outputs[i].name.c_str());
            dl->AddText(ImGui::GetFont(), fs, ImVec2(p.x - 12 * z - ts.x, p.y - fs * 0.5f), labelCol,
                        info.outputs[i].name.c_str(), nullptr, 0.0f, &clip);
        }
        bool hov = hoverPin_.node == n.id && hoverPin_.output && hoverPin_.pin == i;
        drawPin(p, info.outputs[i].type, hov);
    }

    for (int i = 0; i < int(info.params.size()); ++i) {
        const ImRect& box = L.pathBoxes[i];
        if (box.GetWidth() <= 0) continue;
        std::string path = n.paramS(i);
        std::string label = path.empty() ? "Choose image..." : pathToU8(u8ToPath(path).filename());
        bool hovered = false;
        if (interactive && ownsMouse) {
            ImGui::SetCursorScreenPos(box.Min);
            ImGui::PushID(1000 + i);
            if (ImGui::InvisibleButton("##path", box.GetSize())) {
                if (auto p = openFileDialog("Choose image", kImageFilter)) {
                    n.params[i] = *p;
                    changed = true;
                }
            }
            hovered = ImGui::IsItemHovered();
            if (hovered && !path.empty()) ImGui::SetTooltip("%s", path.c_str());
            ImGui::PopID();
        }
        dl->AddRectFilled(box.Min, box.Max, hovered ? IM_COL32(70, 74, 88, 255) : IM_COL32(54, 56, 66, 255), 3 * z);
        if (showText) {
            ImVec4 bclip(box.Min.x + 4 * z, box.Min.y, box.Max.x - 4 * z, box.Max.y);
            dl->AddText(ImGui::GetFont(), fs, ImVec2(box.Min.x + 6 * z, box.GetCenter().y - fs * 0.5f), labelCol,
                        label.c_str(), nullptr, 0.0f, &bclip);
        }
    }

    for (int i = 0; i < int(info.inputs.size()); ++i) {
        ImVec2 p = L.inPins[i];
        const bool linked = g.inputLink(n.id, i) != nullptr;
        const ImRect& box = L.valueBoxes[i];
        const int fp = info.inputs[i].fallbackParam;
        const char* name = info.inputs[i].name.c_str();
        if (!linked && box.GetWidth() > 0) {
            // Unconnected input with a value: show it as an editable field (label inside).
            const bool mine = editing_.node == n.id && editing_.param == fp;
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

bool NodeEditor::deleteSelection(Graph& g, int& preview) {
    bool any = false;
    if (selectedLink_) {
        g.removeLink(selectedLink_);
        selectedLink_ = 0;
        any = true;
    }
    for (int id : selection_) {
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

NodeEditor::Result NodeEditor::draw(Graph& g, int& selected, int& preview) {
    Result r;
    ImGuiIO& io = ImGui::GetIO();
    origin_ = ImGui::GetCursorScreenPos();
    size_ = ImGui::GetContentRegionAvail();
    size_.x = std::max(size_.x, 1.0f);
    size_.y = std::max(size_.y, 1.0f);
    syncOrder(g);
    if (fitPending_) doFrame(g);

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

    // ---- press
    if (bgActivated && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
            if (io.KeyCtrl) {
                preview = (preview == nid) ? 0 : nid;
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
        } else if (int lid = hitLink(g, mouse)) {
            selectedLink_ = lid;
            selection_.clear();
            mode_ = Mode::None;
        } else {
            mode_ = io.KeyShift ? Mode::BoxSelect : Mode::Pan;
        }
    } else if (bgActivated && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        pressPos_ = mouse;
        mode_ = Mode::Pan;
    } else if (bgHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && mode_ == Mode::None) {
        menuPos_ = mouse;
        search_[0] = '\0';
        menuConnect_ = {};
        if (int nid = hitNode(g, mouse)) {
            menuNode_ = nid;
            if (!selection_.count(nid)) selection_ = {nid};
            ImGui::OpenPopup("NodeMenu");
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
                if (insertLink_) {
                    // Splice the dragged node into the wire it was dropped on.
                    Link l = *std::find_if(g.links().begin(), g.links().end(),
                                           [&](const Link& k) { return k.id == insertLink_; });
                    int id = *selection_.begin();
                    g.removeLink(l.id);
                    g.connect(l.fromNode, l.fromPin, id, insertIn_);
                    g.connect(id, insertOut_, l.toNode, l.toPin);
                    r.evalChanged = true;
                }
                insertLink_ = 0;
                mode_ = Mode::None;
            }
            break;
        }
        case Mode::DragLink:
            hoverPin_ = hitPin(g, mouse);
            if (!leftDown) {
                finishLinkDrag(g, r);
                mode_ = Mode::None;
            }
            break;
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
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            if (deleteSelection(g, preview)) r.evalChanged = r.docChanged = true;
        }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D) && duplicateSelection(g)) r.evalChanged = r.docChanged = true;
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A))
            for (const auto& [id, n] : g.nodes()) selection_.insert(id);
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !io.KeyCtrl) fitPending_ = true;
    }

    drawAddMenu(g, r);
    drawNodeMenu(g, preview, r);

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
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##search", "Search nodes...", search_, sizeof(search_));

    const auto& reg = NodeRegistry::instance();
    std::string chosen;
    if (search_[0]) {
        std::string first;
        for (const auto& type : reg.types()) {
            const NodeInfo* inf = reg.find(type);
            if (!compatible(*inf)) continue;
            if (!containsNoCase(inf->displayName, search_) && !containsNoCase(inf->category, search_)) continue;
            if (first.empty()) first = type;
            if (ImGui::MenuItem(inf->displayName.c_str(), inf->category.c_str())) chosen = type;
        }
        if (chosen.empty() && ImGui::IsKeyPressed(ImGuiKey_Enter)) chosen = first;  // Enter picks the first match
    } else {
        std::vector<std::string> cats;
        for (const auto& type : reg.types()) {
            const NodeInfo* inf = reg.find(type);
            if (compatible(*inf) && std::find(cats.begin(), cats.end(), inf->category) == cats.end())
                cats.push_back(inf->category);
        }
        for (const auto& c : cats) {
            if (ImGui::BeginMenu(c.c_str())) {
                for (const auto& type : reg.types()) {
                    const NodeInfo* inf = reg.find(type);
                    if (inf->category == c && compatible(*inf) && ImGui::MenuItem(inf->displayName.c_str()))
                        chosen = type;
                }
                ImGui::EndMenu();
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
    if (ImGui::MenuItem("Duplicate", "Ctrl+D") && duplicateSelection(g)) r.evalChanged = r.docChanged = true;
    if (ImGui::MenuItem(preview == menuNode_ ? "Stop Previewing" : "Preview", "Ctrl+Click")) {
        preview = (preview == menuNode_) ? 0 : menuNode_;
        r.previewChanged = true;
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete", "Del") && deleteSelection(g, preview)) r.evalChanged = r.docChanged = true;
    ImGui::EndPopup();
}
