#pragma once
#include <array>

#include "ui/ImageView.h"

class Node;

// Lightroom-style on-image controls for the selected node, drawn over the Result viewer:
//  - Crop: frame with a guide overlay (Lightroom's Thirds, Golden Spiral and others), corner/edge
//    handles, drag inside to move, drag outside to straighten (Angle).
//  - Perspective: a fine grid, and Guided Upright's guides (drag to draw one, drag its ends to
//    move them, Alt+click removes one).
//  - Linear Gradient: Start/End lines, drag either end or the middle.
//  - Radial Gradient / Box Mask / Ellipse Mask: outline with centre, size and rotation handles.
//  - Brush Mask: paint with the left mouse, Alt+drag erases, [ and ] change the size.
//  - Spot Removal: click to add a spot, drag a spot or its source to move it, drag its edge to
//    resize; Alt+click or Delete removes one, [ and ] change the size.
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
    bool wantsCtrlWheel() const override;

    // Lightroom's crop guide overlays, cycled with O (Shift+O turns the asymmetric ones).
    enum CropGuide { Grid = 0, Thirds, Diagonal, Triangle, GoldenRatio, GoldenSpiral, AspectRatios, kCropGuideCount };
    static const char* cropGuideName(int g);
    int cropGuide = Thirds;
    int cropGuideTurn = 0;  // orientation: bit 0 mirrors left-right, bit 1 top-bottom
    void cycleCropGuide(bool turn) {
        if (turn) cropGuideTurn = (cropGuideTurn + 1) % 4;
        else cropGuide = (cropGuide + 1) % kCropGuideCount;
    }

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
    bool updateSpots(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active);
    bool updatePerspective(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active);
    bool updatePanZoom(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hovered, bool active);
    void setParam(int i, float v);

    Node* node_ = nullptr;
    const GLTexture* mask_ = nullptr;
    int drag_ = -1;          // handle being dragged, -1 none
    float grab_[8] = {};     // param values / offsets captured when the drag started
    float grabX_ = 0, grabY_ = 0;
    bool changed_ = false;
    // Perspective: the transform guides are drawn through, kept while dragging so a guide stays
    // under the mouse while the image straightens behind it.
    std::array<double, 9> toShown_{}, toInput_{};
    bool creating_ = false;  // the guide being dragged was just drawn
    bool spotClick_ = false;  // a spot was just added and its source not dragged (yet)
};

// Lightroom's Loupe Overlay for the Result viewer: a grid and a pair of guides (a horizontal and
// a vertical line, dragged by either line or where they cross). Draws under the selected node's
// controls, which get the mouse first.
class LoupeOverlay : public ImageOverlay {
public:
    bool grid = false, guides = false;
    float gridSize = 50.0f;  // screen pixels between grid lines
    float guideX = 0.5f, guideY = 0.5f;  // image-relative
    ImageOverlay* inner = nullptr;
    bool any() const { return grid || guides; }
    bool update(ImDrawList* dl, const ImVec2& imgMin, const ImVec2& imgMax, bool hovered, bool active) override;

private:
    int drag_ = -1;  // 0 vertical line, 1 horizontal, 2 both
};
