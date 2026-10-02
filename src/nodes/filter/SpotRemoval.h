#pragma once
#include <vector>

#include "nodes/NodeUtil.h"

// Lightroom's Spot Removal: circles whose contents are replaced from another part of the image.
// Clone copies the source as it is; Heal copies its texture and matches the target's colour and
// brightness, so the patch blends in (a membrane fill of the difference along the circle's edge).
struct Spot {
    float x = 0.5f, y = 0.5f;    // target centre, image-relative (0..1 across, 0..1 down)
    float sx = 0.6f, sy = 0.5f;  // source centre
    float radius = 0.03f;        // fraction of the image's long edge
    float feather = 0.5f;        // 0 hard edge .. 1 soft from the centre
    float opacity = 1.0f;
    bool heal = true;  // else Clone
};

class SpotRemovalNode : public Node {
public:
    NODELAB_NODE({"filter.spot_removal", "Spot Removal", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Enum("Mode", 0, {"Heal", "Clone"}), ParamDesc::Float("Size", 0.03f, 0.002f, 0.3f),
                   ParamDesc::Float("Feather", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Opacity", 1.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;

    void saveExtra(nlohmann::json& j) const override;
    void loadExtra(const nlohmann::json& j) override;
    std::string signatureExtra() const override;

    std::vector<Spot> spots;
    // The spot the viewer is editing (-1 none); its Mode, Size, Feather and Opacity are the
    // params, so the Inspector edits it. Not saved.
    int active = -1;

    // Adds a spot at (u, v) with the current params and a source beside it, inside the image
    // (aspect = width / height); returns its index.
    int addSpot(float u, float v, float aspect);
    // Copies the active spot's settings into the params (on selecting it).
    void loadActive();
    // Copies the params into the active spot; true if it changed.
    bool storeActive();
};

// Applies spots to img, which is the part at (x0, y0) of a fullW x fullH image. `linear`: heal
// matches brightness by ratio (scene-linear values) instead of difference. Exposed for tests.
void removeSpots(Image& img, const std::vector<Spot>& spots, bool linear, int x0 = 0, int y0 = 0, int fullW = 0,
                 int fullH = 0);
