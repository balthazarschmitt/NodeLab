// Custom Inspector layouts for the Lightroom-style develop nodes: grouped Basic sliders, the
// Color Mixer's band tabs, Color Grading's colour wheels, and the Brush Mask's stroke tools.
#include "ui/NodeInspectors.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "core/ColorMath.h"
#include "io/ImageIO.h"
#include "ml/Models.h"
#include "ml/Onnx.h"
#include "nodes/filter/SpotRemoval.h"
#include "nodes/matte/AutoMask.h"
#include "nodes/matte/MatteNodes.h"
#include "ui/ViewerOverlay.h"

int autoToneRequest = 0;

namespace {

constexpr float kPi = 3.14159265f;

ImU32 hueColor(float hueDeg, float s, float v, int alpha = 255) {
    float rgb[3];
    colormath::hsvToRgb(hueDeg / 360.0f, s, v, rgb[0], rgb[1], rgb[2]);
    return IM_COL32(int(rgb[0] * 255), int(rgb[1] * 255), int(rgb[2] * 255), alpha);
}

// Sliders tinted with a band's colour, like Lightroom's HSL panel.
void pushBandColor(float hueDeg) {
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, hueColor(hueDeg, 0.75f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, hueColor(hueDeg, 0.6f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, hueColor(hueDeg, 0.45f, 0.3f));
}

// ---------------------------------------------------------------- Basic

void basic(Node& n, const ParamRow& row) {
    row(0);
    ImGui::SeparatorText("White Balance");
    row(1), row(2);
    ImGui::SeparatorText("Tone");
    // Lightroom's Auto, at the top of the Tone group.
    if (ImGui::SmallButton("Auto")) autoToneRequest = n.id;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Set Exposure, Contrast, Highlights, Shadows, Whites and Blacks from the image's tones");
    for (int i = 3; i <= 8; ++i) row(i);
    ImGui::SeparatorText("Presence");
    for (int i = 9; i <= 13; ++i) row(i);
}

// ---------------------------------------------------------------- Color Mixer

void colorMixer(Node& n, const ParamRow& row) {
    static const float kHues[8] = {0, 30, 60, 120, 180, 225, 270, 315};
    static const char* kWhat[3] = {"Hue", "Saturation", "Luminance"};
    row(0);
    if (!ImGui::BeginTabBar("##mixer")) return;
    for (int t = 0; t < 4; ++t) {
        if (!ImGui::BeginTabItem(t < 3 ? kWhat[t] : "All")) continue;
        for (int group = 0; group < 3; ++group) {
            if (t < 3 && group != t) continue;
            if (t == 3) ImGui::SeparatorText(kWhat[group]);
            for (int b = 0; b < 8; ++b) {
                pushBandColor(kHues[b]);
                row(1 + group * 8 + b);
                ImGui::PopStyleColor(3);
            }
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    (void)n;
}

// ---------------------------------------------------------------- Color Grading

// Hue disc: triangles fanning from a grey centre to the saturated rim, hue by angle (0 = red at
// the right, counter-clockwise).
void drawHueDisc(ImDrawList* dl, ImVec2 c, float r) {
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    const int seg = 72;
    dl->PrimReserve(seg * 3, seg * 3);
    for (int i = 0; i < seg; ++i) {
        const float a0 = 2 * kPi * i / seg, a1 = 2 * kPi * (i + 1) / seg;
        dl->PrimVtx(c, uv, IM_COL32(110, 110, 110, 255));
        dl->PrimVtx(ImVec2(c.x + std::cos(a0) * r, c.y - std::sin(a0) * r), uv, hueColor(a0 * 180 / kPi, 0.7f, 0.85f));
        dl->PrimVtx(ImVec2(c.x + std::cos(a1) * r, c.y - std::sin(a1) * r), uv, hueColor(a1 * 180 / kPi, 0.7f, 0.85f));
    }
    dl->AddCircle(c, r, IM_COL32(20, 20, 24, 255), 0, 2.0f);
}

// Hue/saturation wheel: angle is hue (0 = red at the right, counter-clockwise), distance from the
// centre is saturation. Drag to set both; double-click resets saturation; Shift drags finely.
bool colorWheel(const char* id, Node& n, int hueParam, int satParam, float size) {
    ImGui::PushID(id);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##wheel", ImVec2(size, size));
    const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + size * 0.5f, p0.y + size * 0.5f);
    const float r = size * 0.5f - 4.0f;
    drawHueDisc(dl, c, r);
    dl->AddLine(ImVec2(c.x - 4, c.y), ImVec2(c.x + 4, c.y), IM_COL32(40, 40, 40, 200));
    dl->AddLine(ImVec2(c.x, c.y - 4), ImVec2(c.x, c.y + 4), IM_COL32(40, 40, 40, 200));

    bool changed = false;
    float hue = n.paramF(hueParam), sat = n.paramF(satParam);
    const ImGuiIO& io = ImGui::GetIO();
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        sat = 0.0f;
        changed = true;
    } else if (active && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        float dx = io.MousePos.x - c.x, dy = io.MousePos.y - c.y;
        if (io.KeyShift && !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // Fine adjustment: move the current point by a fraction of the mouse motion.
            const float a = hue * kPi / 180, d = sat / 100 * r;
            dx = std::cos(a) * d + io.MouseDelta.x * 0.2f;
            dy = -std::sin(a) * d + io.MouseDelta.y * 0.2f;
        }
        const float d = std::hypot(dx, dy);
        sat = std::min(d / r, 1.0f) * 100.0f;
        if (d > 0.5f) {
            hue = std::atan2(-dy, dx) * 180.0f / kPi;
            if (hue < 0) hue += 360.0f;
        }
        changed = true;
    }
    if (changed) {
        n.params[hueParam] = std::clamp(hue, 0.0f, 360.0f);
        n.params[satParam] = std::clamp(sat, 0.0f, 100.0f);
    }
    const float a = hue * kPi / 180, d = sat / 100 * r;
    const ImVec2 pt(c.x + std::cos(a) * d, c.y - std::sin(a) * d);
    dl->AddLine(c, pt, IM_COL32(255, 255, 255, 120));
    dl->AddCircleFilled(pt, 5.0f, hueColor(hue, sat / 100.0f, 1.0f));
    dl->AddCircle(pt, 5.0f, IM_COL32(255, 255, 255, 255), 0, 1.5f);
    dl->AddCircle(pt, 6.5f, IM_COL32(0, 0, 0, 200), 0, 1.0f);
    if (hovered || active) ImGui::SetTooltip("Hue %.0f  Saturation %.0f\nShift: fine, double-click: reset", hue, sat);
    ImGui::PopID();
    return changed;
}

// One wheel with its title above and Luminance slider below, in a fixed-width column.
bool gradingColumn(const char* name, Node& n, int first, float size) {
    bool changed = false;
    ImGui::BeginGroup();
    const float tw = ImGui::CalcTextSize(name).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (size - tw) * 0.5f));
    ImGui::AlignTextToFramePadding();  // same baseline whether or not the column follows SameLine
    ImGui::TextUnformatted(name);
    changed |= colorWheel(name, n, first, first + 1, size);
    float lum = n.paramF(first + 2);
    ImGui::PushID(name);
    ImGui::SetNextItemWidth(size);
    if (ImGui::SliderFloat("##lum", &lum, -100.0f, 100.0f, "Lum %.0f", ImGuiSliderFlags_AlwaysClamp)) {
        n.params[first + 2] = lum;
        changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s Luminance (double-click the wheel to reset its colour)", name);
    ImGui::PopID();
    ImGui::EndGroup();
    return changed;
}

bool colorGrading(Node& n, const ParamRow& row) {
    bool changed = row(0);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    // Three wheels in a row when there is room, like Lightroom's 3-way view; Global centred below.
    const float size = std::clamp((avail - spacing * 2) / 3.0f, 70.0f, 150.0f);
    const int perRow = std::max(1, int((avail + spacing) / (size + spacing)));
    static const char* kZones[3] = {"Shadows", "Midtones", "Highlights"};
    for (int z = 0; z < 3; ++z) {
        if (z % perRow != 0) ImGui::SameLine();
        changed |= gradingColumn(kZones[z], n, 1 + z * 3, size);
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (std::min(avail, perRow * (size + spacing)) - size) * 0.5f));
    changed |= gradingColumn("Global", n, 10, size);
    ImGui::Spacing();
    changed |= row(13);
    changed |= row(14);
    if (ImGui::CollapsingHeader("Values")) {
        for (int i = 1; i <= 12; ++i) {
            // Sliders under each wheel's hue get that zone's current colour.
            if ((i - 1) % 3 == 0) pushBandColor(n.paramF(i));
            changed |= row(i);
            if ((i - 1) % 3 == 0) ImGui::PopStyleColor(3);
        }
    }
    return changed;
}

// ---------------------------------------------------------------- Brush Mask

bool brushMask(BrushMaskNode& n, const ParamRow& row) {
    bool changed = false;
    for (int i = 0; i < int(n.info().params.size()); ++i) changed |= row(i);
    ImGui::Spacing();
    ImGui::Text("%d stroke%s", int(n.strokes.size()), n.strokes.size() == 1 ? "" : "s");
    ImGui::BeginDisabled(n.strokes.empty());
    ImGui::SameLine();
    if (ImGui::Button("Remove Last")) {
        n.strokes.pop_back();
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Strokes")) {
        n.strokes.clear();
        changed = true;
    }
    ImGui::EndDisabled();
    return changed;
}

}  // namespace

// ---------------------------------------------------------------- Color Key

// Color Key's Hue as a colour wheel: click or drag on it to pick the hue. The keyed region (Hue
// +- Hue Range, from Sat Min to Sat Max out from the grey centre) is outlined on it.
bool colorKey(Node& n, const ParamRow& row) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float size = std::clamp(avail, 60.0f, 170.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - size) * 0.5f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##keyWheel", ImVec2(size, size));
    const bool active = ImGui::IsItemActive(), hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p0.x + size * 0.5f, p0.y + size * 0.5f);
    const float r = size * 0.5f - 4.0f;
    drawHueDisc(dl, c, r);

    bool changed = false;
    const ImGuiIO& io = ImGui::GetIO();
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        n.resetParam(0);
        changed = true;
    } else if (active && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        const float dx = io.MousePos.x - c.x, dy = io.MousePos.y - c.y;
        if (std::hypot(dx, dy) > 2.0f) {
            float hue = std::atan2(-dy, dx) * 180.0f / kPi;
            if (hue < 0) hue += 360.0f;
            n.params[0] = std::clamp(hue, 0.0f, 360.0f);
            changed = true;
        }
    }

    // The keyed region: an annular sector, filled faintly and outlined.
    const float hue = n.paramF(0), range = n.paramF(1);
    const float r0 = std::clamp(n.paramF(2), 0.0f, 1.0f) * r, r1 = std::clamp(n.paramF(3), 0.0f, 1.0f) * r;
    const bool whole = range >= 180.0f;
    const float a0 = (whole ? 0.0f : hue - range) * kPi / 180, a1 = (whole ? 360.0f : hue + range) * kPi / 180;
    const int seg = std::max(2, int(std::ceil((a1 - a0) / (2 * kPi) * 72)));
    auto at = [&](float a, float d) { return ImVec2(c.x + std::cos(a) * d, c.y - std::sin(a) * d); };
    // Without anti-aliasing, or each translucent quad's fringe shows as a seam.
    const ImDrawListFlags flags = dl->Flags;
    dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
    for (int i = 0; i < seg; ++i) {
        const float b0 = a0 + (a1 - a0) * i / seg, b1 = a0 + (a1 - a0) * (i + 1) / seg;
        dl->AddQuadFilled(at(b0, r0), at(b0, r1), at(b1, r1), at(b1, r0), IM_COL32(255, 255, 255, 40));
    }
    dl->Flags = flags;
    for (int i = 0; i <= seg; ++i) dl->PathLineTo(at(a0 + (a1 - a0) * i / seg, r1));
    if (r0 > 0.5f || whole)
        for (int i = seg; i >= 0; --i) dl->PathLineTo(at(a0 + (a1 - a0) * i / seg, r0));
    else
        dl->PathLineTo(c);
    if (whole) dl->PathStroke(IM_COL32(255, 255, 255, 200), 0, 1.5f);
    else dl->PathStroke(IM_COL32(255, 255, 255, 200), ImDrawFlags_Closed, 1.5f);

    // The picked hue: a handle on the rim in its own colour.
    const float ha = hue * kPi / 180;
    const ImVec2 pt = at(ha, r);
    dl->AddLine(c, pt, IM_COL32(255, 255, 255, 120));
    dl->AddCircleFilled(pt, 6.0f, hueColor(hue, 1.0f, 1.0f));
    dl->AddCircle(pt, 6.0f, IM_COL32(255, 255, 255, 255), 0, 1.5f);
    dl->AddCircle(pt, 7.5f, IM_COL32(0, 0, 0, 200), 0, 1.0f);
    if (hovered || active) ImGui::SetTooltip("Hue %.0f\nClick or drag to pick the hue, double-click to reset", hue);

    pushBandColor(hue);
    row(0);
    ImGui::PopStyleColor(3);
    for (int i = 1; i < int(n.params.size()); ++i) row(i);
    return changed;
}

// ---------------------------------------------------------------- Select Subject / Sky

// The model's status, and its download (with the runtime the first time), like Lightroom's
// "download AI models" prompt. Installing bumps ml::generation(), which re-evaluates the graph.
static void autoMask(AutoMaskNode& n, const ParamRow& row) {
    for (int i = 0; i < int(n.params.size()); ++i) row(i);
    ImGui::Spacing();
    const std::string id = n.model().id;
    const ml::ModelSpec* spec = ml::findModel(id);
    if (!spec) return;
    const ml::InstallState st = ml::installState();
    ImGui::PushTextWrapPos(0.0f);
    if (st.running) {
        const bool ours = st.id == id;
        ImGui::TextDisabled("%s", ours ? "Downloading the model..." : "Another model is downloading...");
        char text[64];
        std::snprintf(text, sizeof text, "%.0f / %.0f MB", st.done / 1e6, st.total / 1e6);
        ImGui::ProgressBar(st.total ? float(double(st.done) / double(st.total)) : 0.0f, ImVec2(-1, 0), text);
        if (ImGui::Button("Cancel")) ml::cancelInstall();
    } else if (!ml::available(id)) {
        ImGui::TextUnformatted("This node needs an AI model, which isn't part of NodeLab.exe. Until it is "
                               "downloaded the mask is empty.");
        ImGui::TextDisabled("%s (%s licence)", spec->source, spec->license);
        if (st.id == id && !st.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", st.error.c_str());
        char label[64];
        std::snprintf(label, sizeof label, "Download (%.0f MB)", ml::downloadSize(id) / 1e6);
        if (ImGui::Button(label)) ml::startInstall(id);
    } else {
        const AutoMaskNode::Progress pr = AutoMaskNode::progress();
        if (pr.running && pr.model == id) {
            char text[64];
            if (pr.loading)
                std::snprintf(text, sizeof text, "Loading the model... %.0f s", pr.seconds);
            else
                std::snprintf(text, sizeof text, "%.0f s of about %.0f s", pr.seconds, pr.expected);
            ImGui::TextUnformatted(pr.seconds > pr.expected * 1.5 && pr.seconds > 10
                                       ? "Running the model, longer than usual (is memory full?)"
                                       : "Running the model...");
            // Time, as the runtime reports no progress: held short of the end when it runs long.
            ImGui::ProgressBar(float(std::min(pr.seconds / std::max(pr.expected, 1.0), 0.95)), ImVec2(-1, 0), text);
            ImGui::TextDisabled("Until it finishes, the mask is empty (or a similar picture's). You can keep editing.");
        } else if (pr.running) {
            ImGui::TextDisabled("Waiting for another AI mask to finish...");
        }
        const std::string err = n.lastError();
        if (!err.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", err.c_str());
        ImGui::TextDisabled("%s, on the %s. %s licence.", spec->source, ml::useGpu() ? "GPU when it can" : "CPU",
                            spec->license);
        ImGui::TextDisabled("The model runs once per picture (it can take a minute on a laptop CPU); "
                            "its results are cached.");
    }
    ImGui::PopTextWrapPos();
}

bool drawNodeInspector(Node& n, const ParamRow& row, bool& changed) {
    if (NodeOverlay::supports(n)) {
        const char* hint = n.info().type == "xform.crop"
                               ? "Edit the crop on the Result viewer: drag the frame, its corners or edges; drag outside it to straighten."
                           : dynamic_cast<BrushMaskNode*>(&n)
                               ? "Paint on the Result viewer. Alt+paint erases, [ and ] change the brush size, O toggles the overlay."
                           : dynamic_cast<SpotRemovalNode*>(&n)
                               ? "Click a blemish on the Result viewer to add a spot (drag right away to pick its source). "
                                 "Drag a spot or its source to move it and its edge to resize it; Alt+click or Delete "
                                 "removes it. The settings below apply to the selected spot and new ones."
                               : "Drag the handles on the Result viewer. O toggles the red mask overlay.";
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", hint);
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
    }
    const std::string& t = n.info().type;
    if (t == "color.basic") {
        basic(n, row);
    } else if (t == "color.color_mixer") {
        colorMixer(n, row);
    } else if (t == "color.color_grading") {
        changed |= colorGrading(n, row);
    } else if (t == "conv.color_key") {
        changed |= colorKey(n, row);
    } else if (auto* brush = dynamic_cast<BrushMaskNode*>(&n)) {
        changed |= brushMask(*brush, row);
    } else if (auto* spots = dynamic_cast<SpotRemovalNode*>(&n)) {
        for (int i = 0; i < int(n.params.size()); ++i) row(i);
        ImGui::Spacing();
        ImGui::TextDisabled("%d %s%s", int(spots->spots.size()), spots->spots.size() == 1 ? "spot" : "spots",
                            spots->active >= 0 ? " (one selected)" : "");
        ImGui::BeginDisabled(spots->spots.empty());
        ImGui::SameLine();
        if (ImGui::Button("Remove All")) {
            spots->spots.clear();
            spots->active = -1;
            changed = true;
        }
        ImGui::EndDisabled();
    } else if (auto* am = dynamic_cast<AutoMaskNode*>(&n)) {
        autoMask(*am, row);
    } else if (t == "io.image_input") {
        for (int i = 0; i < int(n.params.size()); ++i) row(i);
        // Which profile Embedded Profile applies, so a P3 or Adobe RGB photo is recognisable.
        if (n.paramVisible(5) && n.paramB(5)) {
            const std::string profile = embeddedProfileInfo(n.paramS(0));
            if (!profile.empty()) ImGui::TextDisabled("Profile: %s", profile.c_str());
        }
    } else {
        return false;
    }
    return true;
}
