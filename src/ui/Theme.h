#pragma once
// Colour themes (Edit > Preferences > Themes), like Blender's: a few built-in presets and custom
// themes made by editing any colour. A theme sets ImGui's style colours from a handful of key
// colours (on top of ImGui's dark or light base) and the colours the Node Editor and the image
// panels draw with.
#include <string>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

namespace theme {

// Key interface colours. A preset leaves a key unset to keep the base style's colours for it
// (so Classic is exactly ImGui's dark style). With Control set, buttons, list highlights and tabs
// are neutral greys from it and the accent marks only state (checks, selection, primary buttons);
// without it they are tinted with the accent, as before.
enum UiKey { Background, TitleBar, Frame, Text, Accent, Border, Control, kUiKeys };

// Colours drawn directly (Node Editor, image panels).
enum Col {
    Canvas, Grid, NodeBody, NodeOutline, Selection, TitleText, LabelText,
    Field, FieldHover, FieldText, FieldValue, Button, ButtonHover, SliderFill, SliderFillHover, CheckFill,
    WireImage, WireChannel, WireNumber, PreviewTitle, MutedTitle,
    CatInputOutput, CatColor, CatMix, CatConverter, CatFilter, CatTransform, CatMatte, CatTexture,
    CatUtility, CatGroup, CatOther,
    ImageBackground,
    kCols
};

struct Theme {
    std::string name;
    bool light = false;           // base style: ImGui's light instead of dark
    bool uiSet[kUiKeys] = {};     // which keys override the base style
    ImVec4 ui[kUiKeys] = {};
    ImU32 col[kCols] = {};

    nlohmann::json toJson() const;
    // Missing entries come from Classic. A theme saved under a built-in preset's name loads that
    // preset as it is now, so built-in themes follow the app's updates.
    static Theme fromJson(const nlohmann::json& j);
};

const std::vector<Theme>& presets();  // [0] is the default, Studio

// The theme in use. apply() sets ImGui's colours from it (call after changing it).
Theme& current();
void apply();

inline ImU32 col(Col c) { return current().col[c]; }
ImU32 categoryColor(const std::string& category);

// Labels for the editors.
const char* uiKeyName(int k);
const char* colName(int c);
// The colour a key shows in the editor: its override, or what the base style uses for it.
ImVec4 uiValue(const Theme& t, int k);

}  // namespace theme
