#pragma once
// The interface's type scale, icon font and metrics (colours are in Theme.h). Sizes follow a
// 4 px grid at 100% UI scale, multiplied by the monitor's DPI scale and the UI Scale preference.
//
// ImGui sizes a font by its line height, and Segoe UI's line is 1.33 em: body text at 17 px is
// 12.8 px in CSS terms (the concept mockups' 13 px).
#include <imgui.h>

namespace style {

struct Fonts {
    ImFont* body = nullptr;      // 17 px, with the Lucide icons merged in
    ImFont* small = nullptr;     // 15 px: captions, status bar, sub-headings, also with icons
    ImFont* semibold = nullptr;  // 17 px Segoe UI Semibold: section and panel titles
    ImFont* heading = nullptr;   // 20 px semibold
};
const Fonts& fonts();

// UI Scale preference (1 = 100%). Changing it rebuilds the fonts before the next frame.
float uiScale();
void setUiScale(float s);
inline constexpr float kUiScales[] = {0.8f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f};

// DPI scale times UI scale: multiply hand-placed pixel sizes by this.
float scale();

// Whether fonts need building (at start and after a UI scale change).
bool fontsDirty();
// Builds the font atlas and sets style metrics. Call outside a frame; the caller re-uploads the
// font texture. `dpi` is the monitor's content scale.
void build(float dpi);
// Sets ImGui's sizes (padding, rounding, spacing). build() calls it; theme changes reset only colours.
void applyMetrics();

}  // namespace style
