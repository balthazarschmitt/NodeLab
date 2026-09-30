#include "ui/ParamWidgets.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/Curve.h"
#include "core/Ramp.h"

// ---------------------------------------------------------------- curves

bool curveEditor(const char* id, nlohmann::json& curves, const std::vector<std::string>& keySpec) {
    std::vector<std::string> keyStore, labelStore;
    bool hueStrip = false;
    for (const auto& k : keySpec) {
        if (k == "@hue") {
            hueStrip = true;
            continue;
        }
        auto colon = k.find(':');
        keyStore.push_back(k.substr(0, colon));
        labelStore.push_back(colon == std::string::npos ? k : k.substr(colon + 1));
    }
    if (keyStore.empty()) {
        keyStore = {"master", "r", "g", "b"};
        labelStore = {"Master", "R", "G", "B"};
    }
    const int nk = int(keyStore.size());
    std::vector<const char*> keys, labels;
    for (int c = 0; c < nk; ++c) keys.push_back(keyStore[c].c_str()), labels.push_back(labelStore[c].c_str());
    static const ImU32 palette[4] = {IM_COL32(235, 235, 240, 255), IM_COL32(230, 90, 90, 255), IM_COL32(90, 210, 100, 255),
                                     IM_COL32(100, 140, 240, 255)};
    // Standard tone curves are colored per channel; custom channels (e.g. hue curves) use white.
    std::vector<ImU32> cols(nk, palette[0]);
    if (keySpec.empty())
        for (int c = 0; c < 4; ++c) cols[c] = palette[c];
    if (!curves.is_object()) curves = keySpec.empty() ? defaultCurves() : nlohmann::json::object();

    ImGui::PushID(id);
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID chanKey = ImGui::GetID("chan"), dragKey = ImGui::GetID("drag");
    int chan = std::clamp(st->GetInt(chanKey, 0), 0, nk - 1);
    bool changed = false;

    for (int c = 0; c < nk; ++c) {
        if (c) ImGui::SameLine();
        if (ImGui::RadioButton(labels[c], chan == c)) st->SetInt(chanKey, chan = c);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) {
        if (hueStrip) {  // hue curves are neutral when flat at 0.5
            nlohmann::json flat = nlohmann::json::array();
            for (int i = 0; i <= 6; ++i) flat.push_back({i / 6.0f, 0.5f});
            curves[keys[chan]] = flat;
        } else {
            curves[keys[chan]] = curveToJson(identityCurve());
        }
        changed = true;
    }

    CurvePoints pts = curveFromJson(curves.contains(keys[chan]) ? curves[keys[chan]] : nlohmann::json());

    // Full panel width, and no taller than the visible height, so the whole curve is on screen in a
    // short Inspector instead of hanging off the bottom (then it is wider than tall, which is fine
    // for editing). Adding the scroll offset keeps the size stable while the panel scrolls.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float w = std::clamp(avail.x, 160.0f, 560.0f);
    const float fitH = avail.y + ImGui::GetScrollY() - ImGui::GetStyle().ItemSpacing.y;
    const float h = std::clamp(fitH, 120.0f, std::min(w, 400.0f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    ImGui::InvisibleButton("##curve", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto toScreen = [&](float x, float y) { return ImVec2(p0.x + x * w, p1.y - y * h); };
    auto toCurve = [&](ImVec2 m) {
        return std::array<float, 2>{std::clamp((m.x - p0.x) / w, 0.0f, 1.0f), std::clamp((p1.y - m.y) / h, 0.0f, 1.0f)};
    };
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    int hoverPt = -1;
    for (int i = 0; i < int(pts.size()); ++i)
        if (ImLengthSqr(toScreen(pts[i][0], pts[i][1]) - mouse) < 64.0f) hoverPt = i;

    int drag = st->GetInt(dragKey, -1);
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hoverPt >= 0) {
            drag = hoverPt;
        } else {
            auto c = toCurve(mouse);
            pts.push_back(c);
            std::sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
            drag = int(std::find(pts.begin(), pts.end(), c) - pts.begin());
            changed = true;
        }
        st->SetInt(dragKey, drag);
    }
    if (active && drag >= 0 && drag < int(pts.size()) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        auto c = toCurve(mouse);
        // Keep x between neighbours so the point keeps its index while dragging.
        float lo = drag > 0 ? pts[drag - 1][0] + 0.005f : 0.0f;
        float hi = drag + 1 < int(pts.size()) ? pts[drag + 1][0] - 0.005f : 1.0f;
        c[0] = std::clamp(c[0], lo, std::max(lo, hi));
        if (c != pts[drag]) {
            pts[drag] = c;
            changed = true;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) st->SetInt(dragKey, -1);
    if (hovered && hoverPt >= 0 && pts.size() > 2 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        pts.erase(pts.begin() + hoverPt);
        hoverPt = -1;
        changed = true;
    }

    // draw
    dl->AddRectFilled(p0, p1, IM_COL32(24, 24, 28, 255));
    if (hueStrip) {
        // Hue along x (what each point of the curve affects), dimmed so the curve stays readable.
        const int segs = 36;
        for (int k = 0; k < segs; ++k) {
            ImVec4 c0 = ImColor::HSV(float(k) / segs, 0.7f, 0.45f), c1 = ImColor::HSV(float(k + 1) / segs, 0.7f, 0.45f);
            float x0 = p0.x + w * k / segs, x1 = p0.x + w * (k + 1) / segs;
            dl->AddRectFilledMultiColor(ImVec2(x0, p1.y - 14), ImVec2(x1, p1.y), ImGui::GetColorU32(c0), ImGui::GetColorU32(c1),
                                        ImGui::GetColorU32(c1), ImGui::GetColorU32(c0));
        }
        dl->AddLine(ImVec2(p0.x, p0.y + h * 0.5f), ImVec2(p1.x, p0.y + h * 0.5f), IM_COL32(90, 90, 100, 255));
    }
    for (int k = 1; k < 4; ++k) {
        float t = k / 4.0f;
        dl->AddLine(ImVec2(p0.x + t * w, p0.y), ImVec2(p0.x + t * w, p1.y), IM_COL32(48, 48, 56, 255));
        dl->AddLine(ImVec2(p0.x, p0.y + t * h), ImVec2(p1.x, p0.y + t * h), IM_COL32(48, 48, 56, 255));
    }
    if (!hueStrip) dl->AddLine(ImVec2(p0.x, p1.y), ImVec2(p1.x, p0.y), IM_COL32(70, 70, 80, 255));
    for (int c = 0; c < nk; ++c) {
        if (c == chan) continue;
        CurvePoints other = curveFromJson(curves.contains(keys[c]) ? curves[keys[c]] : nlohmann::json());
        if (isIdentityCurve(other)) continue;
        ImVec2 prev;
        for (int k = 0; k <= 64; ++k) {
            float x = k / 64.0f;
            ImVec2 p = toScreen(x, evalCurve(other, x));
            if (k) dl->AddLine(prev, p, (cols[c] & 0x00FFFFFF) | 0x60000000, 1.0f);
            prev = p;
        }
    }
    ImVec2 prev;
    for (int k = 0; k <= 128; ++k) {
        float x = k / 128.0f;
        ImVec2 p = toScreen(x, evalCurve(pts, x));
        if (k) dl->AddLine(prev, p, cols[chan], 2.0f);
        prev = p;
    }
    for (int i = 0; i < int(pts.size()); ++i) {
        ImVec2 p = toScreen(pts[i][0], pts[i][1]);
        bool hot = i == hoverPt || i == st->GetInt(dragKey, -1);
        dl->AddCircleFilled(p, hot ? 6.0f : 4.5f, hot ? IM_COL32(255, 255, 255, 255) : cols[chan]);
        dl->AddCircle(p, hot ? 6.0f : 4.5f, IM_COL32(10, 10, 12, 255));
    }
    dl->AddRect(p0, p1, IM_COL32(70, 70, 80, 255));
    // Delayed and hidden while dragging, so the tip never covers the curve being edited.
    if (!active && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("Click: add point   Drag: move   Right-click: delete");

    if (changed) curves[keys[chan]] = curveToJson(pts);
    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------- color ramp

bool rampEditor(const char* id, nlohmann::json& rampJson) {
    ColorRamp ramp = rampFromJson(rampJson, false);
    ImGui::PushID(id);
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID selKey = ImGui::GetID("sel"), dragKey = ImGui::GetID("drag");
    int sel = std::clamp(st->GetInt(selKey, 0), 0, int(ramp.stops.size()) - 1);
    bool changed = false;

    const char* interps[] = {"Linear", "Constant", "Ease", "Smooth"};
    ImGui::SetNextItemWidth(120);
    if (ImGui::Combo("Interpolation", &ramp.interp, interps, 4)) changed = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Flip")) {
        for (auto& s : ramp.stops) s.pos = 1.0f - s.pos;
        changed = true;
    }

    const float w = std::max(160.0f, ImGui::GetContentRegionAvail().x);
    const float barH = 28.0f, markH = 14.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + barH);
    ImGui::InvisibleButton("##ramp", ImVec2(w, barH + markH));
    const bool active = ImGui::IsItemActive();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Checker behind for alpha.
    for (float x = p0.x; x < p1.x; x += 8)
        for (float y = p0.y; y < p1.y; y += 8)
            dl->AddRectFilled(ImVec2(x, y), ImVec2(std::min(x + 8, p1.x), std::min(y + 8, p1.y)),
                              (int((x - p0.x) / 8) + int((y - p0.y) / 8)) % 2 ? IM_COL32(90, 90, 90, 255) : IM_COL32(60, 60, 60, 255));
    const int segs = 96;
    for (int k = 0; k < segs; ++k) {
        float c0[4], c1[4];
        ColorRamp sorted = ramp;
        sorted.sort();
        sorted.eval(float(k) / segs, c0);
        sorted.eval(float(k + 1) / segs, c1);
        auto col = [](const float* c) { return ImGui::GetColorU32(ImVec4(c[0], c[1], c[2], c[3])); };
        float x0 = p0.x + w * k / segs, x1 = p0.x + w * (k + 1) / segs;
        dl->AddRectFilledMultiColor(ImVec2(x0, p0.y), ImVec2(x1, p1.y), col(c0), col(c1), col(c1), col(c0));
    }
    dl->AddRect(p0, p1, IM_COL32(20, 20, 24, 255));

    auto markerX = [&](const RampStop& s) { return p0.x + s.pos * w; };
    int hover = -1;
    for (int i = 0; i < int(ramp.stops.size()); ++i)
        if (std::fabs(markerX(ramp.stops[i]) - mouse.x) < 6 && mouse.y >= p0.y && mouse.y <= p1.y + markH) hover = i;

    if (ImGui::IsItemActivated()) {
        if (hover >= 0) {
            sel = hover;
        } else {
            // Add a stop at the click, using the current color there.
            RampStop s;
            s.pos = std::clamp((mouse.x - p0.x) / w, 0.0f, 1.0f);
            ColorRamp sorted = ramp;
            sorted.sort();
            sorted.eval(s.pos, s.c);
            ramp.stops.push_back(s);
            sel = int(ramp.stops.size()) - 1;
            changed = true;
        }
        st->SetInt(dragKey, 1);
    }
    if (active && st->GetInt(dragKey, 0) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        float pos = std::clamp((mouse.x - p0.x) / w, 0.0f, 1.0f);
        if (pos != ramp.stops[sel].pos) {
            ramp.stops[sel].pos = pos;
            changed = true;
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) st->SetInt(dragKey, 0);

    for (int i = 0; i < int(ramp.stops.size()); ++i) {
        float x = markerX(ramp.stops[i]);
        const auto& c = ramp.stops[i].c;
        ImU32 fill = ImGui::GetColorU32(ImVec4(c[0], c[1], c[2], 1.0f));
        ImVec2 a(x, p1.y + 1), b(x - 6, p1.y + markH), d(x + 6, p1.y + markH);
        dl->AddTriangleFilled(a, b, d, fill);
        dl->AddTriangle(a, b, d, i == sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(10, 10, 12, 255), i == sel ? 2.0f : 1.0f);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p1.y), i == sel ? IM_COL32(255, 255, 255, 180) : IM_COL32(0, 0, 0, 90));
    }

    // Selected stop controls
    RampStop& s = ramp.stops[sel];
    ImGui::SetNextItemWidth(std::min(420.0f, w * 0.75f));
    if (ImGui::ColorEdit4("Color", s.c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_AlphaBar)) changed = true;
    ImGui::SetNextItemWidth(120);
    if (ImGui::SliderFloat("Position", &s.pos, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) changed = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(ramp.stops.size() <= 1);
    if (ImGui::SmallButton("Delete stop")) {
        ramp.stops.erase(ramp.stops.begin() + sel);
        sel = std::max(0, sel - 1);
        changed = true;
    }
    ImGui::EndDisabled();

    st->SetInt(selKey, sel);
    // Stored unsorted so the selected index stays stable while dragging past neighbours;
    // evaluation sorts.
    if (changed) {
        nlohmann::json stops = nlohmann::json::array();
        for (const auto& t : ramp.stops) stops.push_back({t.pos, t.c[0], t.c[1], t.c[2], t.c[3]});
        rampJson = {{"interp", ramp.interp}, {"stops", stops}};
    }
    ImGui::PopID();
    return changed;
}
