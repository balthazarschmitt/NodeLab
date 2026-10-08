#pragma once
#include <array>
#include <string>
#include <vector>

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

// A composition guide of one's own (Preferences > Viewer): columns x rows across the image, with
// its diagonals, a centre mark and a safe-area frame if asked.
struct CustomGuide {
    std::string name = "Custom";
    int columns = 4, rows = 4;
    bool diagonals = false, center = false;
    float safeArea = 0;  // a frame this many percent in from each edge, as video's title safe; 0 none
};

// Lightroom's Loupe Overlay for the Result viewer: a grid, a pair of guides (a horizontal and a
// vertical line, dragged by either line or where they cross) and a composition guide. Draws under
// the selected node's controls, which get the mouse first.
class LoupeOverlay : public ImageOverlay {
public:
    bool grid = false, guides = false;
    float gridSize = 50.0f;  // screen pixels between grid lines
    float guideX = 0.5f, guideY = 0.5f;  // image-relative
    // Composition guide over the whole image: one of the crop tool's (NodeOverlay::CropGuide),
    // or custom[guide - NodeOverlay::kCropGuideCount].
    bool composition = false;
    int guide = NodeOverlay::Thirds;
    int turn = 0;  // orientation of the asymmetric ones, as NodeOverlay::cropGuideTurn
    float opacity = 0.5f;
    std::vector<CustomGuide> custom;
    int guideCount() const { return NodeOverlay::kCropGuideCount + int(custom.size()); }
    const char* guideName(int g) const {
        return g < NodeOverlay::kCropGuideCount ? NodeOverlay::cropGuideName(g)
               : g < guideCount()               ? custom[g - NodeOverlay::kCropGuideCount].name.c_str()
                                                : "";
    }
    // Shows the next guide (Off after the last), or turns the current one.
    void cycleGuide(bool turnIt) {
        if (turnIt) turn = (turn + 1) % 4;
        else if (!composition) composition = true, guide = 0;
        else if (++guide >= guideCount()) composition = false, guide = 0;
    }
    ImageOverlay* inner = nullptr;
    bool any() const { return grid || guides || composition; }
    bool update(ImDrawList* dl, const ImVec2& imgMin, const ImVec2& imgMax, bool hovered, bool active) override;

private:
    int drag_ = -1;  // 0 vertical line, 1 horizontal, 2 both
};
