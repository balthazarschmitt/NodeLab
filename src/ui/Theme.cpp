#include "ui/Theme.h"

#include <algorithm>

namespace theme {
namespace {

constexpr const char* kUiNames[kUiKeys] = {"Background", "Title Bar", "Widget", "Text", "Accent", "Border", "Button"};
constexpr const char* kUiIds[kUiKeys] = {"background", "titleBar", "widget", "text", "accent", "border", "control"};

constexpr const char* kColNames[kCols] = {
    "Canvas", "Grid", "Node Body", "Node Outline", "Selection", "Node Title Text", "Socket Label",
    "Field", "Field Hovered", "Field Label", "Field Value", "Button", "Button Hovered", "Slider", "Slider Hovered",
    "Checkbox",
    "Image Wire", "Channel Wire", "Number Wire", "Previewed Node", "Muted Node",
    "Input / Output", "Color", "Mix", "Converter", "Filter", "Transform", "Matte", "Texture",
    "Utility", "Group", "Other",
    "Image Background",
};
constexpr const char* kColIds[kCols] = {
    "canvas", "grid", "nodeBody", "nodeOutline", "selection", "titleText", "labelText",
    "field", "fieldHover", "fieldText", "fieldValue", "button", "buttonHover", "slider", "sliderHover",
    "checkbox",
    "wireImage", "wireChannel", "wireNumber", "previewTitle", "mutedTitle",
    "catInputOutput", "catColor", "catMix", "catConverter", "catFilter", "catTransform", "catMatte", "catTexture",
    "catUtility", "catGroup", "catOther",
    "imageBackground",
};

ImVec4 rgb(int r, int g, int b) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f); }
ImVec4 alpha(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }
ImVec4 lerp(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}
// Towards white (or black, for negative amounts) by `amount`.
ImVec4 shade(ImVec4 c, float amount) {
    return amount >= 0 ? lerp(c, ImVec4(1, 1, 1, c.w), amount) : lerp(c, ImVec4(0, 0, 0, c.w), -amount);
}

void setUi(Theme& t, UiKey k, ImVec4 v) {
    t.uiSet[k] = true;
    t.ui[k] = v;
}

// Refractory's colours before 1.4 (ImGui's dark style), kept as the Classic preset.
Theme classic() {
    Theme t;
    t.name = "Classic";
    ImU32* c = t.col;
    c[Canvas] = IM_COL32(30, 30, 36, 255);
    c[Grid] = IM_COL32(44, 44, 52, 255);
    c[NodeBody] = IM_COL32(40, 40, 46, 245);
    c[NodeOutline] = IM_COL32(18, 18, 22, 255);
    c[Selection] = IM_COL32(240, 196, 100, 255);
    c[TitleText] = IM_COL32(245, 245, 250, 255);
    c[LabelText] = IM_COL32(212, 212, 218, 255);
    c[Field] = IM_COL32(26, 26, 30, 255);
    c[FieldHover] = IM_COL32(34, 34, 40, 255);
    c[FieldText] = IM_COL32(222, 222, 228, 255);
    c[FieldValue] = IM_COL32(240, 240, 245, 255);
    c[Button] = IM_COL32(54, 56, 66, 255);
    c[ButtonHover] = IM_COL32(70, 74, 88, 255);
    c[SliderFill] = IM_COL32(60, 88, 146, 255);
    c[SliderFillHover] = IM_COL32(78, 108, 170, 255);
    c[CheckFill] = IM_COL32(90, 130, 210, 255);
    c[WireImage] = IM_COL32(236, 184, 72, 255);
    c[WireChannel] = IM_COL32(176, 176, 190, 255);
    c[WireNumber] = IM_COL32(96, 160, 236, 255);
    c[PreviewTitle] = IM_COL32(196, 122, 38, 255);
    c[MutedTitle] = IM_COL32(92, 58, 58, 255);
    c[CatInputOutput] = IM_COL32(56, 108, 78, 255);
    c[CatColor] = IM_COL32(64, 88, 148, 255);
    c[CatMix] = IM_COL32(104, 74, 136, 255);
    c[CatConverter] = IM_COL32(42, 108, 118, 255);
    c[CatFilter] = IM_COL32(120, 70, 60, 255);
    c[CatTransform] = IM_COL32(110, 96, 48, 255);
    c[CatMatte] = IM_COL32(70, 70, 110, 255);
    c[CatTexture] = IM_COL32(126, 76, 104, 255);
    c[CatUtility] = IM_COL32(70, 76, 84, 255);
    c[CatGroup] = IM_COL32(40, 120, 60, 255);
    c[CatOther] = IM_COL32(80, 80, 92, 255);
    c[ImageBackground] = IM_COL32(24, 24, 27, 255);
    return t;
}

// The default since 1.4: Blender-like neutral greys, an amber accent kept for state and the main
// action, and node colours muted so the photo is the brightest thing on screen.
Theme studio() {
    Theme t = classic();
    t.name = "Studio";
    setUi(t, Background, rgb(40, 40, 40));
    setUi(t, TitleBar, rgb(29, 29, 29));
    setUi(t, Frame, rgb(58, 58, 58));
    setUi(t, Text, rgb(228, 228, 228));
    setUi(t, Accent, rgb(232, 145, 58));
    setUi(t, Border, rgb(22, 22, 22));
    setUi(t, Control, rgb(70, 70, 70));
    ImU32* c = t.col;
    c[Canvas] = IM_COL32(29, 29, 29, 255);
    c[Grid] = IM_COL32(40, 40, 40, 255);
    c[NodeBody] = IM_COL32(48, 48, 48, 245);
    c[NodeOutline] = IM_COL32(16, 16, 16, 255);
    c[Selection] = IM_COL32(240, 162, 79, 255);
    c[TitleText] = IM_COL32(240, 240, 240, 255);
    c[LabelText] = IM_COL32(214, 214, 214, 255);
    c[Field] = IM_COL32(58, 58, 58, 255);
    c[FieldHover] = IM_COL32(68, 68, 68, 255);
    c[FieldText] = IM_COL32(200, 200, 200, 255);
    c[FieldValue] = IM_COL32(242, 242, 242, 255);
    c[Button] = IM_COL32(70, 70, 70, 255);
    c[ButtonHover] = IM_COL32(84, 84, 84, 255);
    c[SliderFill] = IM_COL32(74, 90, 120, 255);
    c[SliderFillHover] = IM_COL32(88, 106, 140, 255);
    c[CheckFill] = IM_COL32(232, 145, 58, 255);
    c[WireImage] = IM_COL32(216, 194, 90, 255);
    c[WireChannel] = IM_COL32(160, 160, 160, 255);
    c[WireNumber] = IM_COL32(108, 143, 216, 255);
    c[PreviewTitle] = IM_COL32(196, 112, 36, 255);
    c[MutedTitle] = IM_COL32(88, 60, 60, 255);
    c[CatInputOutput] = IM_COL32(52, 110, 80, 255);
    c[CatColor] = IM_COL32(64, 92, 150, 255);
    c[CatMix] = IM_COL32(134, 90, 54, 255);
    c[CatConverter] = IM_COL32(40, 104, 120, 255);
    c[CatFilter] = IM_COL32(124, 72, 64, 255);
    c[CatTransform] = IM_COL32(112, 98, 52, 255);
    c[CatMatte] = IM_COL32(100, 76, 146, 255);
    c[CatTexture] = IM_COL32(128, 76, 108, 255);
    c[CatUtility] = IM_COL32(74, 74, 74, 255);
    c[CatGroup] = IM_COL32(54, 112, 64, 255);
    c[CatOther] = IM_COL32(80, 80, 80, 255);
    c[ImageBackground] = IM_COL32(32, 32, 32, 255);
    return t;
}

std::vector<Theme> makePresets() {
    std::vector<Theme> out;
    out.push_back(studio());
    out.push_back(classic());

    {  // Blender's default theme: neutral greys, blue accent, its node header colours.
        Theme t = classic();
        t.name = "Blender";
        setUi(t, Background, rgb(48, 48, 48));
        setUi(t, TitleBar, rgb(36, 36, 36));
        setUi(t, Frame, rgb(29, 29, 29));
        setUi(t, Text, rgb(230, 230, 230));
        setUi(t, Accent, rgb(71, 114, 179));
        setUi(t, Border, rgb(20, 20, 20));
        ImU32* c = t.col;
        c[Canvas] = IM_COL32(29, 29, 29, 255);
        c[Grid] = IM_COL32(40, 40, 40, 255);
        c[NodeBody] = IM_COL32(48, 48, 48, 240);
        c[NodeOutline] = IM_COL32(0, 0, 0, 255);
        c[Selection] = IM_COL32(237, 87, 0, 255);
        c[LabelText] = IM_COL32(230, 230, 230, 255);
        c[Field] = IM_COL32(29, 29, 29, 255);
        c[FieldHover] = IM_COL32(40, 40, 40, 255);
        c[FieldText] = IM_COL32(230, 230, 230, 255);
        c[FieldValue] = IM_COL32(250, 250, 250, 255);
        c[Button] = IM_COL32(84, 84, 84, 255);
        c[ButtonHover] = IM_COL32(101, 101, 101, 255);
        c[SliderFill] = IM_COL32(71, 114, 179, 255);
        c[SliderFillHover] = IM_COL32(84, 132, 204, 255);
        c[CheckFill] = IM_COL32(71, 114, 179, 255);
        c[WireImage] = IM_COL32(199, 199, 41, 255);
        c[WireChannel] = IM_COL32(161, 161, 161, 255);
        c[WireNumber] = IM_COL32(99, 99, 199, 255);
        c[PreviewTitle] = IM_COL32(204, 112, 30, 255);
        c[CatInputOutput] = IM_COL32(131, 39, 39, 255);
        c[CatColor] = IM_COL32(108, 105, 0, 255);
        c[CatMix] = IM_COL32(96, 82, 22, 255);
        c[CatConverter] = IM_COL32(36, 98, 131, 255);
        c[CatFilter] = IM_COL32(83, 66, 125, 255);
        c[CatTransform] = IM_COL32(59, 108, 108, 255);
        c[CatMatte] = IM_COL32(151, 64, 64, 255);
        c[CatTexture] = IM_COL32(108, 105, 111, 255);
        c[CatUtility] = IM_COL32(68, 68, 68, 255);
        c[CatGroup] = IM_COL32(59, 102, 10, 255);
        c[ImageBackground] = IM_COL32(40, 40, 40, 255);
        out.push_back(t);
    }
    {  // Lightroom's Develop module: neutral greys that don't tint how a photo is judged.
        Theme t = classic();
        t.name = "Darkroom";
        setUi(t, Background, rgb(38, 38, 38));
        setUi(t, TitleBar, rgb(52, 52, 52));
        setUi(t, Frame, rgb(26, 26, 26));
        setUi(t, Text, rgb(206, 206, 206));
        setUi(t, Accent, rgb(140, 150, 162));
        setUi(t, Border, rgb(16, 16, 16));
        ImU32* c = t.col;
        c[Canvas] = IM_COL32(32, 32, 32, 255);
        c[Grid] = IM_COL32(42, 42, 42, 255);
        c[NodeBody] = IM_COL32(50, 50, 50, 245);
        c[NodeOutline] = IM_COL32(14, 14, 14, 255);
        c[Selection] = IM_COL32(235, 235, 235, 255);
        c[SliderFill] = IM_COL32(96, 102, 110, 255);
        c[SliderFillHover] = IM_COL32(118, 124, 134, 255);
        c[CheckFill] = IM_COL32(170, 176, 186, 255);
        c[Button] = IM_COL32(64, 64, 64, 255);
        c[ButtonHover] = IM_COL32(80, 80, 80, 255);
        c[CatInputOutput] = IM_COL32(70, 92, 78, 255);
        c[CatColor] = IM_COL32(74, 84, 108, 255);
        c[CatMix] = IM_COL32(90, 78, 104, 255);
        c[CatConverter] = IM_COL32(64, 90, 96, 255);
        c[CatFilter] = IM_COL32(104, 78, 72, 255);
        c[CatTransform] = IM_COL32(98, 90, 66, 255);
        c[CatMatte] = IM_COL32(78, 78, 96, 255);
        c[CatTexture] = IM_COL32(104, 80, 94, 255);
        c[CatGroup] = IM_COL32(66, 96, 70, 255);
        c[ImageBackground] = IM_COL32(18, 18, 18, 255);
        out.push_back(t);
    }
    {  // Deep blues.
        Theme t = classic();
        t.name = "Midnight";
        setUi(t, Background, rgb(18, 20, 31));
        setUi(t, TitleBar, rgb(32, 40, 72));
        setUi(t, Frame, rgb(31, 36, 54));
        setUi(t, Text, rgb(224, 228, 244));
        setUi(t, Accent, rgb(116, 140, 255));
        setUi(t, Border, rgb(46, 52, 78));
        ImU32* c = t.col;
        c[Canvas] = IM_COL32(14, 16, 26, 255);
        c[Grid] = IM_COL32(26, 30, 46, 255);
        c[NodeBody] = IM_COL32(30, 34, 52, 245);
        c[NodeOutline] = IM_COL32(8, 9, 16, 255);
        c[Selection] = IM_COL32(255, 210, 120, 255);
        c[Field] = IM_COL32(18, 20, 32, 255);
        c[FieldHover] = IM_COL32(26, 30, 46, 255);
        c[Button] = IM_COL32(46, 52, 78, 255);
        c[ButtonHover] = IM_COL32(60, 68, 102, 255);
        c[SliderFill] = IM_COL32(72, 92, 180, 255);
        c[SliderFillHover] = IM_COL32(92, 114, 214, 255);
        c[CheckFill] = IM_COL32(116, 140, 255, 255);
        c[CatInputOutput] = IM_COL32(40, 112, 96, 255);
        c[CatColor] = IM_COL32(60, 80, 170, 255);
        c[CatMix] = IM_COL32(110, 70, 160, 255);
        c[CatConverter] = IM_COL32(30, 110, 140, 255);
        c[CatFilter] = IM_COL32(140, 64, 84, 255);
        c[CatTransform] = IM_COL32(120, 100, 50, 255);
        c[CatMatte] = IM_COL32(80, 70, 140, 255);
        c[CatTexture] = IM_COL32(140, 70, 130, 255);
        c[CatUtility] = IM_COL32(60, 68, 92, 255);
        c[CatGroup] = IM_COL32(40, 130, 80, 255);
        c[ImageBackground] = IM_COL32(12, 14, 22, 255);
        out.push_back(t);
    }
    {  // Black and white with a yellow accent, for legibility.
        Theme t = classic();
        t.name = "High Contrast";
        setUi(t, Background, rgb(0, 0, 0));
        setUi(t, TitleBar, rgb(40, 40, 40));
        setUi(t, Frame, rgb(28, 28, 28));
        setUi(t, Text, rgb(255, 255, 255));
        setUi(t, Accent, rgb(255, 214, 10));
        setUi(t, Border, rgb(170, 170, 170));
        ImU32* c = t.col;
        c[Canvas] = IM_COL32(0, 0, 0, 255);
        c[Grid] = IM_COL32(36, 36, 36, 255);
        c[NodeBody] = IM_COL32(22, 22, 22, 255);
        c[NodeOutline] = IM_COL32(200, 200, 200, 255);
        c[Selection] = IM_COL32(255, 214, 10, 255);
        c[TitleText] = IM_COL32(255, 255, 255, 255);
        c[LabelText] = IM_COL32(255, 255, 255, 255);
        c[Field] = IM_COL32(0, 0, 0, 255);
        c[FieldHover] = IM_COL32(30, 30, 30, 255);
        c[FieldText] = IM_COL32(255, 255, 255, 255);
        c[FieldValue] = IM_COL32(255, 255, 255, 255);
        c[Button] = IM_COL32(60, 60, 60, 255);
        c[ButtonHover] = IM_COL32(90, 90, 90, 255);
        c[SliderFill] = IM_COL32(150, 120, 0, 255);
        c[SliderFillHover] = IM_COL32(190, 150, 0, 255);
        c[CheckFill] = IM_COL32(255, 214, 10, 255);
        c[WireImage] = IM_COL32(255, 200, 0, 255);
        c[WireChannel] = IM_COL32(230, 230, 230, 255);
        c[WireNumber] = IM_COL32(80, 180, 255, 255);
        c[ImageBackground] = IM_COL32(0, 0, 0, 255);
        out.push_back(t);
    }
    {  // Light: ImGui's light style, light nodes on a light canvas; images stay on mid grey.
        Theme t = classic();
        t.name = "Light";
        t.light = true;
        setUi(t, Background, rgb(236, 236, 240));
        setUi(t, TitleBar, rgb(196, 208, 230));
        setUi(t, Frame, rgb(255, 255, 255));
        setUi(t, Text, rgb(24, 24, 28));
        setUi(t, Accent, rgb(54, 116, 214));
        setUi(t, Border, rgb(176, 176, 186));
        ImU32* c = t.col;
        c[Canvas] = IM_COL32(212, 212, 218, 255);
        c[Grid] = IM_COL32(198, 198, 206, 255);
        c[NodeBody] = IM_COL32(244, 244, 247, 250);
        c[NodeOutline] = IM_COL32(150, 150, 160, 255);
        c[Selection] = IM_COL32(230, 130, 0, 255);
        c[TitleText] = IM_COL32(255, 255, 255, 255);
        c[LabelText] = IM_COL32(40, 40, 46, 255);
        c[Field] = IM_COL32(255, 255, 255, 255);
        c[FieldHover] = IM_COL32(234, 238, 246, 255);
        c[FieldText] = IM_COL32(30, 30, 34, 255);
        c[FieldValue] = IM_COL32(10, 10, 14, 255);
        c[Button] = IM_COL32(222, 224, 230, 255);
        c[ButtonHover] = IM_COL32(204, 208, 218, 255);
        c[SliderFill] = IM_COL32(160, 190, 236, 255);
        c[SliderFillHover] = IM_COL32(136, 172, 230, 255);
        c[CheckFill] = IM_COL32(54, 116, 214, 255);
        c[WireImage] = IM_COL32(214, 150, 20, 255);
        c[WireChannel] = IM_COL32(110, 110, 124, 255);
        c[WireNumber] = IM_COL32(40, 110, 210, 255);
        c[CatInputOutput] = IM_COL32(64, 140, 98, 255);
        c[CatColor] = IM_COL32(74, 108, 186, 255);
        c[CatMix] = IM_COL32(130, 92, 170, 255);
        c[CatConverter] = IM_COL32(46, 134, 148, 255);
        c[CatFilter] = IM_COL32(170, 92, 74, 255);
        c[CatTransform] = IM_COL32(150, 128, 54, 255);
        c[CatMatte] = IM_COL32(94, 94, 150, 255);
        c[CatTexture] = IM_COL32(164, 92, 132, 255);
        c[CatUtility] = IM_COL32(110, 118, 128, 255);
        c[CatGroup] = IM_COL32(52, 150, 76, 255);
        c[CatOther] = IM_COL32(120, 120, 134, 255);
        c[ImageBackground] = IM_COL32(96, 96, 100, 255);
        out.push_back(t);
    }
    return out;
}

Theme& currentStorage() {
    static Theme t = studio();
    return t;
}

// RGB, plus alpha when it isn't opaque (node bodies are slightly see-through).
nlohmann::json colorJson(ImVec4 c) {
    if (c.w < 1.0f) return {c.x, c.y, c.z, c.w};
    return {c.x, c.y, c.z};
}
bool colorFromJson(const nlohmann::json& j, ImVec4& out) {
    if (!j.is_array() || j.size() < 3) return false;
    for (int k = 0; k < 3; ++k)
        if (!j[k].is_number()) return false;
    out = ImVec4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j.size() > 3 && j[3].is_number() ? j[3].get<float>() : 1.0f);
    return true;
}

}  // namespace

const std::vector<Theme>& presets() {
    static const std::vector<Theme> p = makePresets();
    return p;
}

Theme& current() { return currentStorage(); }

const char* uiKeyName(int k) { return kUiNames[k]; }
const char* colName(int c) { return kColNames[c]; }

ImVec4 uiValue(const Theme& t, int k) {
    if (t.uiSet[k]) return t.ui[k];
    ImGuiStyle base;
    if (t.light) ImGui::StyleColorsLight(&base);
    else ImGui::StyleColorsDark(&base);
    const ImVec4* c = base.Colors;
    switch (k) {
        case Background: return alpha(c[ImGuiCol_WindowBg], 1.0f);
        case TitleBar: return c[ImGuiCol_TitleBgActive];
        case Frame: return alpha(c[ImGuiCol_FrameBg], 1.0f);
        case Text: return c[ImGuiCol_Text];
        case Accent: return c[ImGuiCol_ButtonHovered];
        case Border: return alpha(c[ImGuiCol_Border], 1.0f);
        case Control: return alpha(c[ImGuiCol_Button], 1.0f);
    }
    return ImVec4(1, 1, 1, 1);
}

void apply() {
    const Theme& t = current();
    ImGuiStyle& style = ImGui::GetStyle();
    if (t.light) ImGui::StyleColorsLight(&style);
    else ImGui::StyleColorsDark(&style);
    ImVec4* c = style.Colors;
    const ImVec4 bg = uiValue(t, Background);
    const bool dark = !t.light;
    if (t.uiSet[Background]) {
        c[ImGuiCol_WindowBg] = bg;
        c[ImGuiCol_MenuBarBg] = shade(bg, dark ? 0.04f : -0.03f);
        c[ImGuiCol_PopupBg] = alpha(shade(bg, dark ? 0.03f : 0.02f), 0.97f);
        c[ImGuiCol_DockingEmptyBg] = shade(bg, -0.2f);
        c[ImGuiCol_ScrollbarBg] = alpha(shade(bg, -0.1f), 0.53f);
        c[ImGuiCol_TableHeaderBg] = shade(bg, dark ? 0.08f : -0.06f);
    }
    if (t.uiSet[Text]) {
        c[ImGuiCol_Text] = t.ui[Text];
        // Dark text fades faster against a light background: a smaller step keeps dim text
        // readable (about 5:1 on the Light preset's panels, past WCAG's 4.5).
        c[ImGuiCol_TextDisabled] = lerp(t.ui[Text], bg, dark ? 0.45f : 0.33f);
    }
    if (t.uiSet[Frame]) {
        const ImVec4 f = t.ui[Frame];
        const float s = dark ? 1.0f : -1.0f;
        c[ImGuiCol_FrameBg] = f;
        c[ImGuiCol_FrameBgHovered] = shade(f, 0.07f * s);
        c[ImGuiCol_FrameBgActive] = shade(f, 0.13f * s);
        c[ImGuiCol_ScrollbarGrab] = shade(f, 0.18f * s);
        c[ImGuiCol_ScrollbarGrabHovered] = shade(f, 0.26f * s);
        c[ImGuiCol_ScrollbarGrabActive] = shade(f, 0.34f * s);
    }
    if (t.uiSet[TitleBar]) {
        c[ImGuiCol_TitleBgActive] = t.ui[TitleBar];
        c[ImGuiCol_TitleBg] = lerp(t.ui[TitleBar], bg, 0.55f);
        c[ImGuiCol_TitleBgCollapsed] = alpha(c[ImGuiCol_TitleBg], 0.6f);
    }
    if (t.uiSet[Accent]) {
        const ImVec4 a = t.ui[Accent];
        c[ImGuiCol_Button] = alpha(a, 0.40f);
        c[ImGuiCol_ButtonHovered] = a;
        c[ImGuiCol_ButtonActive] = shade(a, -0.15f);
        c[ImGuiCol_Header] = alpha(a, 0.31f);
        c[ImGuiCol_HeaderHovered] = alpha(a, 0.80f);
        c[ImGuiCol_HeaderActive] = a;
        c[ImGuiCol_CheckMark] = a;
        c[ImGuiCol_SliderGrab] = shade(a, -0.1f);
        c[ImGuiCol_SliderGrabActive] = a;
        c[ImGuiCol_SeparatorHovered] = alpha(a, 0.78f);
        c[ImGuiCol_SeparatorActive] = a;
        c[ImGuiCol_ResizeGrip] = alpha(a, 0.20f);
        c[ImGuiCol_ResizeGripHovered] = alpha(a, 0.67f);
        c[ImGuiCol_ResizeGripActive] = alpha(a, 0.95f);
        c[ImGuiCol_TextSelectedBg] = alpha(a, 0.35f);
        c[ImGuiCol_DockingPreview] = alpha(a, 0.70f);
        c[ImGuiCol_NavCursor] = a;
        c[ImGuiCol_PlotHistogram] = a;
        c[ImGuiCol_TabSelectedOverline] = a;
        if (!dark) {
            // Dark text on the solid accent is under 4:1; tints of it keep 7:1 or more.
            c[ImGuiCol_ButtonHovered] = alpha(a, 0.60f);
            c[ImGuiCol_ButtonActive] = alpha(a, 0.80f);
            c[ImGuiCol_HeaderActive] = alpha(a, 0.70f);
        }
    }
    if (t.uiSet[Border]) {
        c[ImGuiCol_Border] = alpha(t.ui[Border], 0.6f);
        c[ImGuiCol_Separator] = t.ui[Border];
        c[ImGuiCol_TableBorderStrong] = t.ui[Border];
        c[ImGuiCol_TableBorderLight] = alpha(t.ui[Border], 0.6f);
    }
    if (t.uiSet[Control]) {
        // Neutral controls: grey buttons and highlights; the accent stays for state.
        const ImVec4 k = t.ui[Control];
        const ImVec4 a = uiValue(t, Accent);
        const float s = dark ? 1.0f : -1.0f;
        c[ImGuiCol_Button] = k;
        c[ImGuiCol_ButtonHovered] = shade(k, 0.08f * s);
        c[ImGuiCol_ButtonActive] = shade(k, 0.16f * s);
        c[ImGuiCol_Header] = alpha(a, 0.28f);
        c[ImGuiCol_HeaderHovered] = shade(k, 0.04f * s);
        c[ImGuiCol_HeaderActive] = alpha(a, 0.42f);
        c[ImGuiCol_SliderGrab] = shade(k, 0.22f * s);
        c[ImGuiCol_SliderGrabActive] = shade(k, 0.32f * s);
        // The selected tab merges with its panel; the others sit on the title bar.
        const ImVec4 title = uiValue(t, TitleBar);
        c[ImGuiCol_TitleBg] = title;
        c[ImGuiCol_TitleBgActive] = title;
        c[ImGuiCol_Tab] = title;
        c[ImGuiCol_TabHovered] = shade(bg, 0.05f * s);
        c[ImGuiCol_TabSelected] = bg;
        c[ImGuiCol_TabSelectedOverline] = a;
        c[ImGuiCol_TabDimmed] = title;
        c[ImGuiCol_TabDimmedSelected] = bg;
        c[ImGuiCol_TabDimmedSelectedOverline] = alpha(a, 0.0f);
        c[ImGuiCol_DockingEmptyBg] = title;
        c[ImGuiCol_MenuBarBg] = title;
        c[ImGuiCol_Separator] = alpha(shade(bg, -0.45f * s), 1.0f);
        c[ImGuiCol_SeparatorHovered] = alpha(a, 0.6f);
        c[ImGuiCol_PopupBg] = alpha(shade(bg, 0.02f * s), 0.98f);
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_TableHeaderBg] = shade(bg, 0.05f * s);
        c[ImGuiCol_TableRowBgAlt] = alpha(shade(bg, 0.03f * s), 1.0f);
    } else if (t.uiSet[Accent] || t.uiSet[TitleBar] || t.uiSet[Background]) {
        // Tabs follow from the header and title colours, as ImGui's own styles derive them.
        c[ImGuiCol_TabHovered] = c[ImGuiCol_HeaderHovered];
        c[ImGuiCol_Tab] = lerp(c[ImGuiCol_Header], c[ImGuiCol_TitleBgActive], 0.80f);
        c[ImGuiCol_TabSelected] = lerp(c[ImGuiCol_HeaderActive], c[ImGuiCol_TitleBgActive], 0.60f);
        c[ImGuiCol_TabDimmed] = lerp(c[ImGuiCol_Tab], c[ImGuiCol_TitleBg], 0.80f);
        c[ImGuiCol_TabDimmedSelected] = lerp(c[ImGuiCol_TabSelected], c[ImGuiCol_TitleBg], 0.40f);
    }
    c[ImGuiCol_WindowBg].w = 1.0f;  // opaque, also for floating viewports
}

ImU32 categoryColor(const std::string& cat) {
    if (cat == "Input / Output") return col(CatInputOutput);
    if (cat == "Color") return col(CatColor);
    if (cat == "Mix") return col(CatMix);
    if (cat == "Converter") return col(CatConverter);
    if (cat == "Filter") return col(CatFilter);
    if (cat == "Transform") return col(CatTransform);
    if (cat == "Matte") return col(CatMatte);
    if (cat == "Texture") return col(CatTexture);
    if (cat == "Utility") return col(CatUtility);
    if (cat == "Group") return col(CatGroup);
    return col(CatOther);
}

nlohmann::json Theme::toJson() const {
    nlohmann::json j{{"name", name}, {"light", light}};
    nlohmann::json u = nlohmann::json::object();
    for (int k = 0; k < kUiKeys; ++k)
        if (uiSet[k]) u[kUiIds[k]] = colorJson(ui[k]);
    j["ui"] = u;
    nlohmann::json n = nlohmann::json::object();
    for (int k = 0; k < kCols; ++k) n[kColIds[k]] = colorJson(ImGui::ColorConvertU32ToFloat4(this->col[k]));
    j["colors"] = n;
    return j;
}

Theme Theme::fromJson(const nlohmann::json& j) {
    Theme t = classic();
    if (!j.is_object()) return t;
    t.name = j.value("name", std::string("Custom"));
    // "NodeLab Dark" was the default before Studio, saved whether or not anyone chose it.
    if (t.name == "NodeLab Dark") return studio();
    for (const Theme& p : presets())
        if (p.name == t.name) return p;
    t.light = j.value("light", false);
    if (const auto u = j.find("ui"); u != j.end() && u->is_object())
        for (int k = 0; k < kUiKeys; ++k)
            if (const auto e = u->find(kUiIds[k]); e != u->end()) t.uiSet[k] = colorFromJson(*e, t.ui[k]);
    if (const auto n = j.find("colors"); n != j.end() && n->is_object())
        for (int k = 0; k < kCols; ++k)
            if (const auto e = n->find(kColIds[k]); e != n->end()) {
                ImVec4 v;
                if (colorFromJson(*e, v)) t.col[k] = ImGui::ColorConvertFloat4ToU32(v);
            }
    return t;
}

}  // namespace theme
