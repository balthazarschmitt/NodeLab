#include "ui/Inspector.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <imgui.h>
#include <imgui_internal.h>

#include "io/ImageIO.h"
#include "io/ImageWrite.h"
#include "io/Paths.h"
#include "nodes/group/GroupNodes.h"
#include "nodes/math/LayerStack.h"
#include "ui/ColorDisplay.h"
#include "ui/Eyedropper.h"
#include "ui/FileDialog.h"
#include "ui/GuideWindow.h"
#include "ui/NodeInspectors.h"
#include "ui/ParamWidgets.h"
#include "ui/Icons.h"
#include "ui/SliderTrack.h"
#include "ui/Style.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"


namespace {

ImGuiID g_typeNext = 0;  // number field to open for typing on its next frame

// Call before the field: opens it for typing when its menu's Edit Value asked for that.
void beginNumberField(const char* label) {
    if (g_typeNext && g_typeNext == ImGui::GetID(label)) {
        ImGui::SetKeyboardFocusHere();
        g_typeNext = 0;
    }
}

// Call right after the field (the last item): Backspace over it, or right-click > Reset to
// Default, resets it. Returns true if it changed the param.
bool endNumberField(Node& node, int i) {
    bool changed = false;
    const ImGuiID id = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered();
    if (hovered && !ImGui::IsItemActive() && !ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        node.resetParam(i);
        changed = true;
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##numberMenu");
    if (ImGui::BeginPopup("##numberMenu")) {
        if (ImGui::MenuItem("Reset to Default", "Double-click")) {
            node.resetParam(i);
            changed = true;
        }
        if (ImGui::MenuItem("Edit Value", "Ctrl+click")) g_typeNext = id;
        ImGui::EndPopup();
    }
    return changed;
}

// "-0.0" reads as a change from the default; drop the sign of a value that prints as zero.
void dropNegativeZero(char* buf) {
    if (buf[0] == '-' && std::strspn(buf + 1, "0.") == std::strlen(buf + 1)) std::memmove(buf, buf + 1, std::strlen(buf));
}

// A number param as Lightroom's slider rows: the name on the left inside the frame, the value on
// the right, and a fill from zero (or the minimum) to the value. Dragging moves the value from
// where it was, the frame's width covering the slider range (Shift faster, Alt slower);
// double-click resets it and Ctrl+click types, as in Lightroom and Blender. Track sliders
// (Temperature, Hue...) show their colours and a notch instead of the fill.
bool numberSlider(Node& node, int i, const std::string& label, float width, bool showName) {
    const ParamDesc& d = node.info().params[i];
    const bool isInt = d.kind == ParamKind::Int;
    const ImGuiStyle& s = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 q(p.x + width, p.y + h);
    const ImGuiID id = ImGui::GetID(label.c_str());
    const bool typing = ImGui::TempInputIsActive(id);
    // ImGui's drag fields type on a double-click; here it resets, so the second click is spotted
    // before the field and the field runs without text input for that frame.
    const bool reset = !typing && ImGui::GetCurrentContext()->HoveredIdPreviousFrame == id &&
                       ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                       ImGui::GetIO().MouseClickedCount[ImGuiMouseButton_Left] == 2;
    const bool track = d.track != SliderTrack::None;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int colours = 0;
    if (track) {
        // The coloured track goes under a see-through frame; hover and drag still lighten it.
        slidertrack::draw(dl, p, q, d, theme::col(theme::Field), s.FrameRounding);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 22));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(255, 255, 255, 34));
        colours = 3;
    }
    // ImGui's own text is hidden (the name and value are drawn below), except while typing.
    if (!typing) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0)), ++colours;
    const float speed = (d.max - d.min) / std::max(width, 1.0f);
    const ImGuiSliderFlags flags = ImGuiSliderFlags_AlwaysClamp | (reset ? ImGuiSliderFlags_NoInput : 0);
    const bool wide = d.hardMax > d.max || d.hardMin < d.min;  // math inputs: any value
    const char* fmt = isInt ? "%d" : (!wide && d.max - d.min >= 20.0f) ? "%.1f" : "%.3f";
    ImGui::SetNextItemWidth(width);
    bool changed = false;
    float v = 0.0f;
    if (isInt) {
        int iv = node.paramI(i);
        changed = ImGui::DragInt(label.c_str(), &iv, std::max(speed, 0.02f), int(d.hardMin), int(d.hardMax), fmt, flags);
        if (changed) node.params[i] = iv;
        v = float(iv);
    } else {
        v = node.paramF(i);
        changed = ImGui::DragFloat(label.c_str(), &v, speed, d.hardMin, d.hardMax, fmt, flags);
        if (changed) node.params[i] = v;
    }
    ImGui::PopStyleColor(colours);
    if (reset && ImGui::IsItemActive()) {
        // Let go of the field too, or it keeps dragging while the button is held.
        ImGui::ClearActiveID();
        node.resetParam(i);
        v = isInt ? float(node.paramI(i)) : node.paramF(i);
        changed = true;
    }
    if (typing) return changed;

    const float lo = d.min, hi = d.max;
    const float frac = hi > lo ? std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
    if (track) {
        slidertrack::marker(dl, p.x + width * frac, p.y, q.y, ImGui::GetFontSize() / 17.0f);
    } else if (hi > lo) {
        // Fill from zero for signed ranges (Exposure), from the minimum otherwise (Factor), in the
        // theme's slider colour (Color Mixer tints it with each band's).
        const float zero = lo < 0.0f && hi > 0.0f ? (0.0f - lo) / (hi - lo) : 0.0f;
        float a = p.x + width * zero, b = p.x + width * frac;
        if (a > b) std::swap(a, b);
        if (b - a >= 0.5f) {
            ImDrawFlags corners = (a <= p.x + 0.5f ? ImDrawFlags_RoundCornersLeft : 0) |
                                  (b >= q.x - 0.5f ? ImDrawFlags_RoundCornersRight : 0);
            dl->AddRectFilled(ImVec2(a, p.y), ImVec2(b, q.y), ImGui::GetColorU32(ImGuiCol_SliderGrab, 0.4f),
                              corners ? s.FrameRounding : 0.0f, corners ? corners : ImDrawFlags_RoundCornersNone);
        }
        if (zero > 0.0f) {
            const float x = IM_ROUND(p.x + width * zero);
            dl->AddLine(ImVec2(x, p.y + h * 0.2f), ImVec2(x, q.y - h * 0.2f), ImGui::GetColorU32(ImGuiCol_Text, 0.18f));
        }
    }

    char value[48];
    if (isInt) std::snprintf(value, sizeof value, fmt, int(v));
    else std::snprintf(value, sizeof value, fmt, v);
    dropNegativeZero(value);
    const float defV = d.def.is_number() ? d.def.get<float>() : 0.0f;
    const bool edited = std::fabs(v - defV) > (isInt ? 0.5f : 1e-6f);
    const float ty = p.y + s.FramePadding.y;
    const ImVec2 vs = ImGui::CalcTextSize(value);
    const ImVec4 clip(p.x, p.y, q.x, q.y);
    const ImU32 shadow = IM_COL32(0, 0, 0, 150);
    auto text = [&](float x, ImU32 col, const char* str) {
        // Over a colour track the text gets a shadow, as it may sit on yellow or white.
        if (track) dl->AddText(nullptr, 0.0f, ImVec2(x + 1, ty + 1), shadow, str, nullptr, 0.0f, &clip);
        dl->AddText(nullptr, 0.0f, ImVec2(x, ty), col, str, nullptr, 0.0f, &clip);
    };
    const float vx = q.x - s.FramePadding.x - vs.x;
    text(vx, ImGui::GetColorU32(ImGuiCol_Text), value);
    if (showName) {
        const std::string name = d.name;
        const ImVec4 nameClip(p.x, p.y, vx - s.ItemInnerSpacing.x, q.y);
        // The name is dim at the default and bright, with an accent dot, once changed. Over a
        // colour track a dim name would be lost, so it is only a little dimmer.
        const ImU32 nameCol = edited  ? ImGui::GetColorU32(ImGuiCol_Text)
                              : track ? ImGui::GetColorU32(ImGuiCol_Text, 0.8f)
                                      : ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const float r = std::min(ImGui::GetFontSize() * 0.12f, s.FramePadding.x * 0.3f);
        const float nx = p.x + s.FramePadding.x + r;  // room for the dot
        if (track) dl->AddText(nullptr, 0.0f, ImVec2(nx + 1, ty + 1), shadow, name.c_str(), nullptr, 0.0f, &nameClip);
        dl->AddText(nullptr, 0.0f, ImVec2(nx, ty), nameCol, name.c_str(), nullptr, 0.0f, &nameClip);
        if (edited) dl->AddCircleFilled(ImVec2(p.x + s.FramePadding.x * 0.5f + r * 0.5f, p.y + h * 0.5f), r, ui::accent());
    }
    return changed;
}

// Other params: the name on the left, dim, and the widget in the rest of the row. Returns the
// widget's width. A name too long for the column (a narrow panel, a large UI scale) goes on its
// own line above a full-width widget rather than being cut off.
float leftLabel(const std::string& name, float width) {
    const float labelW = std::floor(width * 0.38f);
    if (ImGui::CalcTextSize(name.c_str()).x > labelW - ImGui::GetStyle().ItemInnerSpacing.x) {
        ImGui::TextDisabled("%s", name.c_str());
        return width;
    }
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    const ImVec4 clip(p.x, p.y, p.x + labelW - ImGui::GetStyle().ItemInnerSpacing.x, p.y + h);
    ImGui::GetWindowDrawList()->AddText(nullptr, 0.0f, ImVec2(p.x, p.y + ImGui::GetStyle().FramePadding.y),
                                        ImGui::GetColorU32(ImGuiCol_TextDisabled), name.c_str(), nullptr, 0.0f, &clip);
    ImGui::Dummy(ImVec2(labelW, h));
    ImGui::SameLine(0, 0);
    return width - labelW;
}

}  // namespace

bool editParam(Node& node, int i, float width, bool compact) {
    const ParamDesc& d = node.info().params[i];
    // Names are drawn by these widgets (or beside them); "##Name" still lets scripts find them.
    const std::string label = "##" + d.name;
    bool changed = false;
    ImGui::PushID(i);
    auto labelled = [&] { ImGui::SetNextItemWidth(compact ? width : leftLabel(d.name, width)); };
    switch (d.kind) {
        case ParamKind::Float:
        case ParamKind::Int:
            beginNumberField(label.c_str());
            changed = numberSlider(node, i, label, width, !compact);
            changed |= endNumberField(node, i);
            break;
        case ParamKind::Bool: {
            bool v = node.paramB(i);
            changed = ImGui::Checkbox(d.name.c_str(), &v);
            if (changed) node.params[i] = v;
            break;
        }
        case ParamKind::Enum: {
            labelled();
            int v = node.paramI(i);
            const char* preview = (v >= 0 && v < int(d.options.size())) ? d.options[v].c_str() : "?";
            if (ImGui::BeginCombo(label.c_str(), preview)) {
                for (int k = 0; k < int(d.options.size()); ++k) {
                    if (ImGui::Selectable(d.options[k].c_str(), k == v)) {
                        node.params[i] = k;
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            break;
        }
        case ParamKind::Path: {
            if (!compact) leftLabel(d.name, width);
            std::string path = node.paramS(i);
            std::string name = path.empty() ? "(none)" : pathToU8(u8ToPath(path).filename());
            if (ImGui::Button("Browse...")) {
                if (auto p = openFileDialog("Choose image", kImageFileFilter)) {
                    chooseImageFile(node, i, *p);
                    changed = true;
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(name.c_str());
            if (!path.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            break;
        }
        case ParamKind::Text: {
            labelled();
            char buf[1024];
            std::snprintf(buf, sizeof(buf), "%s", node.paramS(i).c_str());
            if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) {
                node.params[i] = std::string(buf);
                changed = true;
            }
            break;
        }
        case ParamKind::Curve:
            // Channel buttons share the label's line, leaving more height for the curve.
            ImGui::TextUnformatted(d.name.c_str());
            ImGui::SameLine(0, 16);
            changed = curveEditor("##curves", node.params[i], d.options);
            break;
        case ParamKind::Color: {
            const bool picking = eyedropper().is(node.id, i);
            const char* pickLabel = picking ? "Picking..." : "Pick";
            const ImGuiStyle& s = ImGui::GetStyle();
            const float pickW = ImGui::CalcTextSize(pickLabel).x + s.FramePadding.x * 2 + s.ItemSpacing.x;
            const float w = compact ? width : leftLabel(d.name, width);
            ImGui::SetNextItemWidth(std::max(w - pickW, ImGui::GetFrameHeight() * 3));
            float c[3];
            node.paramC(i, c);
            if (!d.gammaColor) colordisplay::toDisplay(c);
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float;
            if (d.max > 1.0f) flags |= ImGuiColorEditFlags_HDR;
            if (ImGui::ColorEdit3(label.c_str(), c, flags)) {
                if (!d.gammaColor) colordisplay::fromDisplay(c);
                for (int k = 0; k < 3; ++k) c[k] = std::clamp(c[k], d.hardMin, d.hardMax);
                node.params[i] = nlohmann::json::array({c[0], c[1], c[2]});
                changed = true;
            }
            ImGui::SameLine();
            if (picking) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(pickLabel)) eyedropper().toggle(node.id, i);
            if (picking) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Eyedropper: click a pixel in the Original, Result or a viewer,\n"
                                  "or drag a rectangle to use its average colour. Right-click or Esc cancels.");
            break;
        }
        case ParamKind::SavePath: {
            if (!compact) leftLabel(d.name, width);
            std::string path = node.paramS(i);
            if (ImGui::Button("Save as...")) {
                if (auto p = saveFileDialog("File Output", kSaveImageFilter, "png")) {
                    node.params[i] = *p;
                    // File Output: the type picked in the dialog sets the Format.
                    const auto& descs = node.info().params;
                    if (size_t(i) + 1 < descs.size() && descs[i + 1].name == "Format")
                        node.params[i + 1] = int(formatFromPath(*p));
                    changed = true;
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(path.empty() ? "(not set)" : pathToU8(u8ToPath(path).filename()).c_str());
            if (!path.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            break;
        }
        case ParamKind::Ramp:
            ImGui::TextUnformatted(d.name.c_str());
            changed = rampEditor("##ramp", node.params[i]);
            break;
    }
    ImGui::PopID();
    return changed;
}

bool drawGroupInterface(GroupNode& group, Graph& outer) {
    bool changed = false;
    char name[128];
    std::snprintf(name, sizeof(name), "%s", group.name.c_str());
    ImGui::SetNextItemWidth(240 * style::scale());
    if (ImGui::InputText("Group name", name, sizeof(name))) {
        group.name = name;
        group.syncInner();
        changed = true;
    }
    static const char* types[] = {"Image", "Channel", "Number"};
    for (int side = 0; side < 2; ++side) {
        const bool output = side == 1;
        auto& pins = output ? group.outs : group.ins;
        ImGui::SeparatorText(output ? "Outputs" : "Inputs");
        ImGui::PushID(side);
        for (int i = 0; i < int(pins.size()); ++i) {
            ImGui::PushID(i);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s", pins[i].name.c_str());
            ImGui::SetNextItemWidth(150 * style::scale());
            if (ImGui::InputText("##name", buf, sizeof(buf))) {
                pins[i].name = buf;
                group.syncInner();
                changed = true;
            }
            ImGui::SameLine();
            int t = int(pins[i].type);
            ImGui::SetNextItemWidth(ui::comboWidth(types, 3, 100));
            if (ImGui::Combo("##type", &t, types, 3)) {
                group.setPinType(outer, output, i, PinType(t));
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
                group.movePin(outer, output, i, -1);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
                group.movePin(outer, output, i, +1);
                changed = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                group.removePin(outer, output, i);
                changed = true;
                ImGui::PopID();
                break;
            }
            if (!output && pins[i].type != PinType::Image && i < int(group.ranges.size())) {
                // Blender's socket Default / Min / Max: the slider shown on the group node while
                // the input is unconnected.
                GroupNode::InputRange r = group.ranges[size_t(i)];
                float v[3] = {r.def, r.min, r.max};
                ImGui::Indent();
                ImGui::SetNextItemWidth(240 * style::scale());
                if (ImGui::DragFloat3("Default / Min / Max", v, 0.01f, -1e6f, 1e6f, "%.3f")) {
                    // Moving min past max (or back) drags the other along.
                    if (v[1] != r.min) v[2] = std::max(v[2], v[1]);
                    else if (v[2] != r.max) v[1] = std::min(v[1], v[2]);
                    group.setRange(i, {v[0], v[1], v[2]});
                    changed = true;
                }
                ImGui::Unindent();
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton(output ? "Add Output" : "Add Input")) {
            group.addPin(outer, output, {output ? "Result" : "Value", PinType::Image});
            changed = true;
        }
        ImGui::PopID();
    }
    return changed;
}

// Layer Stack's layers, top first as in Photoshop's Layers panel: mode, reorder, remove, add.
// The opacities are params (their sliders show on the node's pins too).
static bool drawLayerStack(LayerStackNode& ls, Graph& g, const std::function<bool(int)>& row) {
    bool changed = false;
    ImGui::TextDisabled("Layer 0 is the bottom. Connecting the top layer adds another.");
    for (int i = ls.layers() - 1; i >= 0; --i) {
        ImGui::PushID(i);
        ImGui::SeparatorText(("Layer " + std::to_string(i) + (ls.layerUsed(g, i) ? "" : " (empty)")).c_str());
        row(i * LayerStackNode::kStride);
        row(i * LayerStackNode::kStride + 1);
        ImGui::BeginDisabled(i == ls.layers() - 1);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
            ls.moveLayer(g, i, +1);
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i == 0);
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
            ls.moveLayer(g, i, -1);
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(ls.layers() <= LayerStackNode::kMinLayers);
        if (ImGui::SmallButton("Remove")) {
            ls.removeLayer(g, i);
            changed = true;
            ImGui::EndDisabled();
            ImGui::PopID();
            break;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(ls.layers() >= LayerStackNode::kMaxLayers);
    if (ImGui::Button("Add Layer")) {
        ls.addLayer(g);
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

bool drawInspector(Graph& g, int selectedNode, GroupNode* owner, Graph* ownerParent, bool embedded) {
    Node* n = g.find(selectedNode);
    if (n) {
        if (auto* grp = dynamic_cast<GroupNode*>(n)) {
            ImGui::Text("Group: %s", grp->name.c_str());
            ImGui::TextDisabled("Tab or double-click to edit its contents, Ctrl+Alt+G to ungroup.");
            ImGui::Separator();
            return drawGroupInterface(*grp, g);
        }
        if (auto* v = dynamic_cast<GroupValueNode*>(n); v && owner && ownerParent) {
            const bool output = v->isOutput();
            auto& pins = output ? owner->outs : owner->ins;
            if (v->pin < 0 || v->pin >= int(pins.size())) return false;
            ImGui::Text("%s", output ? "Value Output" : "Value Input");
            ImGui::TextDisabled("The group's %s socket \"%s\". F2 renames it too.", output ? "output" : "input",
                                pins[size_t(v->pin)].name.c_str());
            ImGui::Separator();
            bool changed = false;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s", pins[size_t(v->pin)].name.c_str());
            ImGui::SetNextItemWidth(240 * style::scale());
            if (ImGui::InputText("Name", buf, sizeof(buf)) && buf[0]) {
                owner->renamePin(output, v->pin, buf);
                changed = true;
            }
            // The socket's type: Number for a slider, Channel for a mask, Image for pixels.
            static const char* kTypes[] = {"Image", "Channel", "Number"};
            int t = int(pins[size_t(v->pin)].type);
            ImGui::SetNextItemWidth(ui::comboWidth(kTypes, 3, 240));
            if (ImGui::Combo("Type", &t, kTypes, 3)) {
                owner->setPinType(*ownerParent, output, v->pin, PinType(t));
                changed = true;
            }
            if (!output && pins[size_t(v->pin)].type != PinType::Image && v->pin < int(owner->ranges.size())) {
                GroupNode::InputRange r = owner->ranges[size_t(v->pin)];
                float vals[3] = {r.def, r.min, r.max};
                ImGui::SetNextItemWidth(240 * style::scale());
                if (ImGui::DragFloat3("Default / Min / Max", vals, 0.01f, -1e6f, 1e6f, "%.3f")) {
                    if (vals[1] != r.min) vals[2] = std::max(vals[2], vals[1]);
                    else if (vals[2] != r.max) vals[1] = std::min(vals[1], vals[2]);
                    owner->setRange(v->pin, {vals[0], vals[1], vals[2]});
                    changed = true;
                }
            }
            if (ImGui::CollapsingHeader("All of the group's sockets")) changed |= drawGroupInterface(*owner, *ownerParent);
            return changed;
        }
        const bool io = dynamic_cast<GroupInputNode*>(n) || dynamic_cast<GroupOutputNode*>(n);
        if (io && owner && ownerParent) {
            ImGui::Text("%s", n->info().displayName.c_str());
            ImGui::TextDisabled("Edit the group's pins here. Tab to leave the group.");
            ImGui::Separator();
            return drawGroupInterface(*owner, *ownerParent);
        }
    }
    if (!n) {
        ImGui::TextDisabled("Select a node to edit its settings.");
        ImGui::TextDisabled("Right-click the canvas to add nodes. Ctrl+click a node to preview it.");
        ImGui::TextDisabled("Drag empty space to pan, wheel to zoom, Shift+drag to box-select, Home to frame all. Help menu lists all shortcuts.");
        return false;
    }
    const NodeInfo& info = n->info();
    if (!embedded) {
        // The node's name (semibold), its category, and the Guide on the right.
        ImGui::AlignTextToFramePadding();
        if (ImFont* f = style::fonts().semibold) ImGui::PushFont(f);
        ImGui::TextUnformatted(n->title().c_str());
        if (style::fonts().semibold) ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", info.category.c_str());
        if (info.type == "conv.expression" || info.type == "conv.image_expression") {
            ImGui::SameLine();
            ImGui::TextDisabled(ICON_INFO);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Variables: r g b a (Image input), in1 in2, x y (pixel), u v (0..1), w h\n"
                                  "Functions: sin cos tan pow sqrt abs floor ceil log ln exp atan2\n"
                                  "           min max clamp mix step smoothstep fract\n"
                                  "Example: mix(r, b, smoothstep(0.3, 0.7, v))");
        }
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight()));
        if (ui::IconButton(ICON_BOOK, "Guide", false, "F1", "What this node does")) openGuide(info.displayName);
        ImGui::Separator();
    }

    if (info.params.empty()) {
        ImGui::TextDisabled("No settings.");
        return false;
    }

    bool changed = false;
    const float width = ImGui::GetContentRegionAvail().x;
    // Scope widget state (selected curve channel, ramp stop) to this node, so selecting another
    // Curves node doesn't inherit the previous one's channel.
    ImGui::PushID(n->id);
    const ParamRow row = [&](int i) {
        if (!n->paramVisible(i)) return false;
        // A param that backs a connected input is overridden by the wire.
        bool driven = false;
        for (int p = 0; p < int(info.inputs.size()); ++p)
            if (info.inputs[p].fallbackParam == i && g.inputLink(n->id, p)) driven = true;
        if (driven) {
            const char* wired = "(wired)";
            const ImGuiStyle& s = ImGui::GetStyle();
            ImGui::BeginDisabled();
            editParam(*n, i, std::max(width - ImGui::CalcTextSize(wired).x - s.ItemSpacing.x, width * 0.5f), false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", wired);
            return false;
        }
        const bool c = editParam(*n, i, width, false);
        changed |= c;
        return c;
    };
    if (auto* ls = dynamic_cast<LayerStackNode*>(n)) changed |= drawLayerStack(*ls, g, row);
    else if (!drawNodeInspector(*n, row, changed, &g))
        drawParamGroups(*n, row);
    ImGui::PopID();
    return changed;
}
