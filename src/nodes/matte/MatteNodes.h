#pragma once
#include <array>
#include <string>
#include <vector>

#include "nodes/NodeUtil.h"

// Painted mask, like Lightroom's Brush. Strokes are painted in the Result viewer while the node
// is selected and saved with the node (not as params, since they grow with every stroke).
class BrushMaskNode : public Node {
public:
    struct Stroke {
        float radius = 0.04f;   // fraction of the image's long edge
        float feather = 0.5f;   // 0 hard edge .. 1 soft from the centre
        float flow = 1.0f;      // coverage added per stroke
        bool erase = false;
        // Lightroom's Auto Mask: paint only over colours like the one under the brush.
        bool autoMask = false;
        std::vector<std::array<float, 2>> pts;  // image-relative (0..1 across, 0..1 down)
    };

    REFRACTORY_NODE({"matte.brush_mask", "Brush Mask", "Matte",
                  // Image (appended, so older links keep their pins): the photo Auto Mask follows.
                  {{"Mask", PinType::Channel}, {"Image", PinType::Image}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Float("Size", 0.04f, 0.002f, 0.3f), ParamDesc::Float("Feather", 0.5f, 0.0f, 1.0f),
                   ParamDesc::Float("Flow", 1.0f, 0.0f, 1.0f), ParamDesc::Bool("Invert", false),
                   ParamDesc::Bool("Auto Mask", false)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;

    void saveExtra(nlohmann::json& j) const override;
    void loadExtra(const nlohmann::json& j) override;
    std::string signatureExtra() const override;

    std::vector<Stroke> strokes;

    // Painting (the viewer calls these). A new stroke takes the current Size, Feather, Flow and
    // Auto Mask.
    void beginStroke(float u, float v, bool erase);
    // Adds a point if it is far enough from the last one; returns true if the mask changed.
    bool extendStroke(float u, float v, int imageW, int imageH);
};

// Rasterizes strokes over an existing mask (w x h, row-major). Exposed for tests. `image` is what
// Auto Mask strokes follow (any size; it is sampled at the same relative position), `encoded`
// whether its values are sRGB-encoded (legacy projects). Without it, Auto Mask does nothing.
void paintStrokes(std::vector<float>& mask, int w, int h, const std::vector<BrushMaskNode::Stroke>& strokes,
                  const Image* image = nullptr, bool encoded = false);
