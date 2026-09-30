#pragma once
#include "graph/Node.h"

// Crop node helpers shared with the viewer's crop overlay.
namespace crop {

constexpr const char* kType = "xform.crop";
// Param indices of the Crop node.
enum { Left = 0, Right, Top, Bottom, ResizeImage, Angle, Aspect, Constrain };

// Width / height of the Aspect option, or 0 for Free. `imageW/H` is used by "Original".
float aspectRatio(int option, int imageW, int imageH);

struct Rect {
    float l = 0, r = 1, t = 0, b = 1;  // fractions of the image
};

// The crop rectangle after the aspect ratio is applied (shrunk around its centre to fit).
Rect effectiveRect(const Node& n, int imageW, int imageH);

}  // namespace crop
