#pragma once
#include <nlohmann/json.hpp>

#include "core/Image.h"

// Scene colour management, like Blender's Render Properties > Color Management.
//
// Scene-linear projects load sRGB images as linear light, so nodes work on unbounded linear
// values (Exposure is a plain multiply, highlights can go above 1). The view transform maps
// them to the display, only in the viewers and when exporting, never inside the graph.
// Legacy projects (saved before 0.7) work on sRGB-encoded values and show them as they are.
struct ColorManagement {
    enum View { Standard, AgX, Raw };
    enum Look { None, Punchy, Greyscale };

    bool linear = false;  // false: legacy display-referred working space
    int view = Standard;
    int look = None;       // AgX only
    float exposure = 0.0f;  // stops, applied in scene-linear before the view transform
    float gamma = 1.0f;     // applied to the display values after it

    static ColorManagement sceneLinear() {
        ColorManagement cm;
        cm.linear = true;
        return cm;
    }
    bool operator==(const ColorManagement&) const = default;

    nlohmann::json toJson() const;
    // Missing or malformed fields keep their defaults; a missing block means legacy.
    static ColorManagement fromJson(const nlohmann::json& j);
};

namespace colormgmt {

inline const char* const kViewNames[] = {"Standard", "AgX", "Raw"};
inline const char* const kLookNames[] = {"None", "Punchy", "Greyscale"};

// Scene-linear Rec.709 RGB to display-encoded sRGB 0..1 (exposure, view transform, look, gamma).
void viewTransform(const ColorManagement& cm, const float in[3], float out[3]);

// The image as the display shows it. Legacy projects return `img` unchanged (already display
// values). Alpha is copied.
ImagePtr displayImage(const ImagePtr& img, const ColorManagement& cm);

// AgX's sigmoid on its log2 encoding, exposed for tests.
float agxContrast(float x);

}  // namespace colormgmt
