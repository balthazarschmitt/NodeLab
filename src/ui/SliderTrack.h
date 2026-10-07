#pragma once
// Coloured slider tracks (ParamDesc::track), like Lightroom's Temperature, Tint and HSL sliders:
// the track shows the colour each value gives, so the slider says which way it pushes.
#include <imgui.h>

#include "graph/Node.h"

namespace slidertrack {

// The track's display (sRGB) colour at t, 0..1 from the slider's left end to its right.
void colorAt(const ParamDesc& d, float t, float rgb[3]);

// Fills [a, b] with the track, blended halfway toward `base` (the field colour) so the label and
// value drawn over it stay readable in dark and light themes.
void draw(ImDrawList* dl, ImVec2 a, ImVec2 b, const ParamDesc& d, ImU32 base, float rounding);

// The value's marker at x: notches on the top and bottom edges of [y0, y1] (Lightroom's), which
// stay clear of the label and value text a full-height grab would cross.
void marker(ImDrawList* dl, float x, float y0, float y1, float scale);

}  // namespace slidertrack
