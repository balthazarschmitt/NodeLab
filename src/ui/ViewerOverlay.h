#pragma once
#include "ui/ImageView.h"

class Node;

// Lightroom-style on-image controls for the selected node, drawn over the Result viewer:
//  - Crop: frame with rule-of-thirds grid, corner/edge handles, drag inside to move, drag
//    outside to straighten (Angle).
//  - Linear Gradient: Start/End lines, drag either end or the middle.
//  - Radial Gradient / Box Mask / Ellipse Mask: outline with centre, size and rotation handles.
//  - Brush Mask: paint with the left mouse, Alt+drag erases, [ and ] change the size.
// With a mask texture set, the mask is tinted over the image (Lightroom's mask overlay).
class NodeOverlay : public ImageOverlay {
public:
    static bool supports(const Node& n);
    static bool isMask(const Node& n);  // shows a mask overlay while selected

    // Called every frame before drawing; node may be null. mask: tinted mask texture or null.
    void set(Node* node, const GLTexture* mask) {
        if (node != node_) drag_ = -1;
        node_ = node;
        mask_ = mask;
    }
    bool update(ImDrawList* dl, const ImVec2& imgMin, const ImVec2& imgMax, bool hovered, bool active) override;

    // True once after the node was edited (the caller marks the document changed).
    bool takeChanged() {
        bool c = changed_;
        changed_ = false;
        return c;
    }

private:
    bool updateCrop(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active);
    bool updateLinear(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active);
    bool updateShape(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active);
    bool updateBrush(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active);
    void setParam(int i, float v);

    Node* node_ = nullptr;
    const GLTexture* mask_ = nullptr;
    int drag_ = -1;          // handle being dragged, -1 none
    float grab_[8] = {};     // param values / offsets captured when the drag started
    float grabX_ = 0, grabY_ = 0;
    bool changed_ = false;
};
