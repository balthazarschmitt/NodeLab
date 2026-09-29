#include "ui/Inspector.h"

#include <cstdio>

#include <imgui.h>

#include "io/Paths.h"
#include "nodes/group/GroupNodes.h"
#include "ui/FileDialog.h"
#include "ui/ParamWidgets.h"

static const char* kImageFilter = "Images|*.png;*.jpg;*.jpeg;*.bmp;*.tga|All files|*.*";

bool editParam(Node& node, int i, float width, bool compact) {
    const ParamDesc& d = node.info().params[i];
    std::string label = compact ? "##" + d.name : d.name;
    bool changed = false;
    ImGui::PushID(i);
    ImGui::SetNextItemWidth(width);
    switch (d.kind) {
        case ParamKind::Float: {
            float v = node.paramF(i);
            if (compact || d.hardMax > d.max || d.hardMin < d.min) {
                // Unbounded (math) values: drag field whose speed follows the soft range.
                changed = ImGui::DragFloat(label.c_str(), &v, (d.max - d.min) / 300.0f, d.hardMin, d.hardMax, "%.3f",
                                           ImGuiSliderFlags_AlwaysClamp);
            } else {
                changed = ImGui::SliderFloat(label.c_str(), &v, d.min, d.max, "%.3f", ImGuiSliderFlags_AlwaysClamp);
            }
            if (changed) node.params[i] = v;
            break;
        }
        case ParamKind::Int: {
            int v = node.paramI(i);
            changed = ImGui::SliderInt(label.c_str(), &v, int(d.min), int(d.max), "%d", ImGuiSliderFlags_AlwaysClamp);
            if (changed) node.params[i] = v;
            break;
        }
        case ParamKind::Bool: {
            bool v = node.paramB(i);
            changed = ImGui::Checkbox(label.c_str(), &v);
            if (changed) node.params[i] = v;
            break;
        }
        case ParamKind::Enum: {
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
            std::string path = node.paramS(i);
            std::string name = path.empty() ? "(none)" : pathToU8(u8ToPath(path).filename());
            if (ImGui::Button("Browse...")) {
                if (auto p = openFileDialog("Choose image", kImageFilter)) {
                    node.params[i] = *p;
                    changed = true;
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(name.c_str());
            if (!path.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
            break;
        }
        case ParamKind::Text: {
            char buf[1024];
            std::snprintf(buf, sizeof(buf), "%s", node.paramS(i).c_str());
            if (ImGui::InputText(label.c_str(), buf, sizeof(buf))) {
                node.params[i] = std::string(buf);
                changed = true;
            }
            break;
        }
        case ParamKind::Curve:
            ImGui::TextUnformatted(d.name.c_str());
            changed = curveEditor("##curves", node.params[i]);
            break;
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
    ImGui::SetNextItemWidth(240);
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
            ImGui::SetNextItemWidth(150);
            if (ImGui::InputText("##name", buf, sizeof(buf))) {
                pins[i].name = buf;
                group.syncInner();
                changed = true;
            }
            ImGui::SameLine();
            int t = int(pins[i].type);
            ImGui::SetNextItemWidth(100);
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

bool drawInspector(Graph& g, int selectedNode, GroupNode* owner, Graph* ownerParent) {
    Node* n = g.find(selectedNode);
    if (n) {
        if (auto* grp = dynamic_cast<GroupNode*>(n)) {
            ImGui::Text("Group: %s", grp->name.c_str());
            ImGui::TextDisabled("Tab or double-click to edit its contents, Ctrl+Alt+G to ungroup.");
            ImGui::Separator();
            return drawGroupInterface(*grp, g);
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
        ImGui::TextDisabled("Drag empty space to pan, wheel to zoom, Shift+drag to box-select, F to frame all.");
        return false;
    }
    const NodeInfo& info = n->info();
    ImGui::Text("%s", info.displayName.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", info.category.c_str());
    if (info.type == "conv.expression" || info.type == "conv.image_expression") {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Variables: r g b a (Image input), in1 in2, x y (pixel), u v (0..1), w h\n"
                              "Functions: sin cos tan pow sqrt abs floor ceil log ln exp atan2\n"
                              "           min max clamp mix step smoothstep fract\n"
                              "Example: mix(r, b, smoothstep(0.3, 0.7, v))");
    }
    ImGui::Separator();

    if (info.params.empty()) {
        ImGui::TextDisabled("No settings.");
        return false;
    }

    bool changed = false;
    const float width = ImGui::GetContentRegionAvail().x * 0.6f;
    for (int i = 0; i < int(info.params.size()); ++i) {
        // A param that backs a connected input is overridden by the wire.
        bool driven = false;
        for (int p = 0; p < int(info.inputs.size()); ++p)
            if (info.inputs[p].fallbackParam == i && g.inputLink(n->id, p)) driven = true;
        if (driven) {
            ImGui::BeginDisabled();
            editParam(*n, i, width, false);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("(wired)");
        } else {
            changed |= editParam(*n, i, width, false);
        }
    }
    return changed;
}
