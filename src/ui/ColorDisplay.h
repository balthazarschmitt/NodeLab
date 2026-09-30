#pragma once
#include "core/ColorMath.h"

// Colour params in scene-linear projects hold linear values, but pickers and swatches should show
// them as the display does, like Blender's colour pickers. App sets `linear` from the project each
// frame; legacy projects store sRGB-encoded values, which are shown as they are.
namespace colordisplay {

inline bool linear = false;

// Stored value -> what the widget shows/edits (alpha untouched).
inline void toDisplay(float* c, int n = 3) {
    if (linear)
        for (int k = 0; k < n; ++k) c[k] = colormath::linearToSrgb(c[k]);
}
inline void fromDisplay(float* c, int n = 3) {
    if (linear)
        for (int k = 0; k < n; ++k) c[k] = colormath::srgbToLinear(c[k]);
}

}  // namespace colordisplay
