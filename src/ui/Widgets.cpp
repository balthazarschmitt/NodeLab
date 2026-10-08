#include "ui/Widgets.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include <imgui_internal.h>

#include "ui/Icons.h"
#include "ui/Style.h"
#include "ui/Theme.h"

namespace ui {
namespace {

// Text colour for an icon at rest: between full and disabled text, so icons recede until hovered.
ImU32 iconColor(bool bright) {
    const ImGuiStyle& s = ImGui::GetStyle();
    if (bright) return ImGui::GetColorU32(ImGuiCol_Text);
    return ImGui::GetColorU32(ImLerp(s.Colors[ImGuiCol_Text], s.Colors[ImGuiCol_TextDisabled], 0.45f));
}

void centredText(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, const char* text) {
    const ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(IM_TRUNC(min.x + (max.x - min.x - ts.x) * 0.5f), IM_TRUNC(min.y + (max.y - min.y - ts.y) * 0.5f)), col, text);
}

// The visible part of "Name##id".
std::string visible(const char* label) {
    const char* end = ImGui::FindRenderedTextEnd(label);
    return std::string(label, end);
}

}  // namespace

ImU32 accent() { return ImGui::ColorConvertFloat4ToU32(theme::uiValue(theme::current(), theme::Accent)); }

float comboWidth(const char* const* items, int count, float minWidth) {
    float w = 0.0f;
    for (int i = 0; i < count; ++i)
        if (items[i]) w = std::max(w, ImGui::CalcTextSize(items[i]).x);
    // Text, its padding on both sides, and the square arrow button.
    const ImGuiStyle& s = ImGui::GetStyle();
    return std::max(minWidth * style::scale(), w + s.FramePadding.x * 2.0f + ImGui::GetFrameHeight());
}

float comboWidth(const char* zeroSeparated, float minWidth) {
    std::vector<const char*> items;
    for (const char* p = zeroSeparated; *p; p += std::strlen(p) + 1) items.push_back(p);
    return comboWidth(items.data(), int(items.size()), minWidth);
}

ImVec2 windowSize(float w, float h) {
    const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
    return ImVec2(std::min(w * style::scale(), work.x * 0.9f), std::min(h * style::scale(), work.y * 0.9f));
}

std::string ellipsize(const std::string& text, float maxWidth) {
    if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;
    const float dots = ImGui::CalcTextSize("...").x;
    // Longest prefix that fits with the dots, by binary search over bytes (UTF-8 lead bytes only).
    size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (ImGui::CalcTextSize(text.data(), text.data() + mid).x + dots <= maxWidth) lo = mid;
        else hi = mid - 1;
    }
    while (lo > 0 && (static_cast<unsigned char>(text[lo]) & 0xC0) == 0x80) --lo;
    return text.substr(0, lo) + "...";
}

bool IconButton(const char* icon, const char* name, bool on, const char* key, const char* tip) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // The name is the button's ID too (never drawn), so scripts can click it by name.
    const bool pressed = ImGui::InvisibleButton(name, ImVec2(h, h));
    const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = ImGui::GetStyle().FrameRounding;
    if (held) dl->AddRectFilled(p, p + ImVec2(h, h), ImGui::GetColorU32(ImGuiCol_ButtonActive), r);
    else if (on) dl->AddRectFilled(p, p + ImVec2(h, h), ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button), r);
    else if (hovered) dl->AddRectFilled(p, p + ImVec2(h, h), ImGui::GetColorU32(ImGuiCol_Button, 0.6f), r);
    centredText(dl, p, p + ImVec2(h, h), iconColor(on || hovered), icon);
    Tooltip(visible(name).c_str(), key, tip);
    return pressed;
}

bool PrimaryButton(const char* label, const ImVec2& size) {
    const ImVec4 a = ImGui::ColorConvertU32ToFloat4(accent());
    // Dark text on a light accent (Studio's amber), light text on a dark one.
    const float lum = 0.2126f * a.x + 0.7152f * a.y + 0.0722f * a.z;
    ImGui::PushStyleColor(ImGuiCol_Button, a);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImLerp(a, ImVec4(1, 1, 1, 1), 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImLerp(a, ImVec4(0, 0, 0, 1), 0.15f));
    ImGui::PushStyleColor(ImGuiCol_Text, lum > 0.45f ? ImVec4(0.09f, 0.06f, 0.02f, 1) : ImVec4(1, 1, 1, 1));
    const bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return pressed;
}

bool Toggle(const char* label, bool* v) {
    const ImGuiStyle& s = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight();
    const float th = IM_TRUNC(h * 0.62f), tw = IM_TRUNC(th * 1.8f);
    const std::string text = visible(label);
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(tw + (ts.x > 0 ? s.ItemInnerSpacing.x + ts.x : 0), h));
    if (pressed) *v = !*v;
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a(p.x, p.y + (h - th) * 0.5f), b(a.x + tw, a.y + th);
    const float rr = th * 0.5f;
    dl->AddRectFilled(a, b, *v ? accent() : ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), rr);
    const float knob = rr - IM_TRUNC(2.0f * style::scale());
    const ImVec2 c(*v ? b.x - rr : a.x + rr, a.y + rr);
    dl->AddCircleFilled(c, knob, *v ? IM_COL32(255, 255, 255, 255) : iconColor(hovered));
    if (ts.x > 0)
        dl->AddText(ImVec2(b.x + s.ItemInnerSpacing.x, p.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(*v ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                    text.c_str());
    return pressed;
}

bool Segmented(const char* id, int* current, const char* const* items, int count, const char* const* names) {
    const ImGuiStyle& s = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight(), pad = IM_TRUNC(2.0f * style::scale());
    ImGui::PushID(id);
    float w = pad;
    float widths[16] = {};
    count = count > 16 ? 16 : count;
    for (int i = 0; i < count; ++i) {
        const bool iconOnly = names && names[i];
        widths[i] = iconOnly ? h : ImGui::CalcTextSize(items[i]).x + s.FramePadding.x * 2;
        w += widths[i];
    }
    w += pad;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + ImVec2(w, h), ImGui::GetColorU32(ImGuiCol_FrameBg), s.FrameRounding);
    bool changed = false;
    float x = p.x + pad;
    for (int i = 0; i < count; ++i) {
        const ImVec2 a(x, p.y + pad), b(x + widths[i], p.y + h - pad);
        ImGui::SetCursorScreenPos(a);
        const std::string label = names && names[i] ? names[i] : items[i];
        if (ImGui::InvisibleButton(label.c_str(), b - a) && *current != i) {
            *current = i;
            changed = true;
        }
        const bool hovered = ImGui::IsItemHovered(), sel = *current == i;
        if (sel || hovered)
            dl->AddRectFilled(a, b, ImGui::GetColorU32(sel ? ImGuiCol_Button : ImGuiCol_FrameBgHovered), s.FrameRounding - 1);
        centredText(dl, a, b, iconColor(sel || hovered), items[i]);
        if (names && names[i]) Tooltip(visible(names[i]).c_str());
        x += widths[i];
    }
    // Reserve the whole control in the layout.
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
    return changed;
}

bool SectionHeader(const char* title, bool* open, bool* enabled, bool* reset) {
    const ImGuiStyle& s = ImGui::GetStyle();
    const float fh = ImGui::GetFrameHeight(), h = IM_TRUNC(fh + 6.0f * style::scale());
    const float w = ImGui::GetContentRegionAvail().x;
    const int buttons = (enabled ? 1 : 0) + (reset ? 1 : 0);
    const float right = buttons * (fh + s.ItemInnerSpacing.x);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(title);

    // The title row toggles the section; scripts click it by its title.
    if (ImGui::InvisibleButton(title, ImVec2(std::max(1.0f, w - right), h))) *open = !*open;
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) dl->AddRectFilled(p, p + ImVec2(w, h), ImGui::GetColorU32(ImGuiCol_FrameBgHovered, 0.5f), s.FrameRounding);
    const bool on = !enabled || *enabled;
    const ImU32 text = ImGui::GetColorU32(on && *open ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    const char* chevron = *open ? ICON_CHEVRON_DOWN : ICON_CHEVRON_RIGHT;
    const ImVec2 cs = ImGui::CalcTextSize(chevron);
    dl->AddText(ImVec2(p.x + s.FramePadding.x * 0.5f, p.y + (h - cs.y) * 0.5f), iconColor(hovered), chevron);
    ImFont* semibold = style::fonts().semibold;
    const std::string label = visible(title);
    const float fs = semibold ? semibold->FontSize : ImGui::GetFontSize();
    dl->AddText(semibold, fs, ImVec2(p.x + s.FramePadding.x * 0.5f + cs.x + s.ItemInnerSpacing.x, p.y + (h - fs) * 0.5f), text,
                label.c_str());

    float bx = p.x + w - right;
    if (enabled) {
        ImGui::SetCursorScreenPos(ImVec2(bx, p.y + (h - fh) * 0.5f));
        const std::string name = std::string(*enabled ? "Bypass " : "Enable ") + label;
        if (IconButton(*enabled ? ICON_EYE : ICON_EYE_OFF, name.c_str(), false)) *enabled = !*enabled;
        bx += fh + s.ItemInnerSpacing.x;
    }
    if (reset) {
        ImGui::SetCursorScreenPos(ImVec2(bx, p.y + (h - fh) * 0.5f));
        *reset = IconButton(ICON_RESET, ("Reset " + label).c_str(), false);
    }
    // Reserve the row, with a hairline under it.
    dl->AddLine(ImVec2(p.x, p.y + h - 0.5f), ImVec2(p.x + w, p.y + h - 0.5f), ImGui::GetColorU32(ImGuiCol_Separator, 0.6f));
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
    return *open;
}

void SubHeading(const char* text) {
    std::string upper = visible(text);
    for (char& c : upper) c = char(std::toupper(static_cast<unsigned char>(c)));
    ImGui::Dummy(ImVec2(0, 2.0f * style::scale()));
    if (ImFont* f = style::fonts().small) ImGui::PushFont(f);
    ImGui::TextDisabled("%s", upper.c_str());
    if (style::fonts().small) ImGui::PopFont();
}

void Tooltip(const char* name, const char* key, const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    if (!ImGui::BeginTooltip()) return;
    if (ImFont* f = style::fonts().semibold) ImGui::PushFont(f);
    ImGui::TextUnformatted(name);
    if (style::fonts().semibold) ImGui::PopFont();
    if (key && *key) {
        ImGui::SameLine(0, ImGui::GetFontSize());
        ImGui::TextDisabled("%s", key);
    }
    if (text && *text) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 18.0f);
        ImGui::TextDisabled("%s", text);
        ImGui::PopTextWrapPos();
    }
    ImGui::EndTooltip();
}

}  // namespace ui
