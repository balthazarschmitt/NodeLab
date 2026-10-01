#pragma once
#include "io/ImageCache.h"
#include "io/RawDecode.h"
#include "nodes/NodeUtil.h"

class ImageInputNode : public Node {
public:
    NODELAB_NODE({"io.image_input", "Image Input", "Input / Output",
                  {},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Path("File"), ParamDesc::Enum("Color Space", 0, {"sRGB", "Linear Rec.709", "Non-Color"}),
                   ParamDesc::Enum("Highlight Reconstruction", 2, {"Clip", "Blend", "Reconstruct"}),
                   ParamDesc::Float("Baseline Exposure", 0.0f, -4.0f, 4.0f),
                   ParamDesc::Bool("Compensate Camera Exposure", false),
                   ParamDesc::Bool("Embedded Profile", true)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;

    // RAW files keep the exposure as shot, which looks about a stop darker than the camera's
    // JPEG: cameras underexpose to protect highlights. Like darktable, choosing a RAW sets a
    // Baseline Exposure of +0.7 EV and undoes the exposure compensation set on the camera. Both
    // start at 0 / off so projects saved before these params render as they did.
    static constexpr float kRawBaselineEV = 0.7f;
    // Sets the file; switching to a RAW from no file or a non-RAW one applies the RAW defaults
    // (returns true then). Moving between RAWs keeps the settings.
    bool chooseFile(const std::string& pathU8);
    // The scale the Baseline Exposure params apply (1 for non-RAW files).
    float exposureGain() const;
    // img scaled by exposureGain() (img itself when that is 1).
    ImagePtr applyExposure(ImagePtr img) const;

    // How the file's values are read. Legacy projects take them as they are, whatever the
    // Color Space; scene-linear ones decode sRGB files to linear light. RAW files are always
    // linear camera data, so Color Space doesn't apply to them.
    // An sRGB-encoded file with an embedded ICC profile (Display P3, Adobe RGB) is decoded through
    // it, like Lightroom, unless Embedded Profile is off.
    ImageCache::Decode decode(bool linearProject) const {
        const bool srgb = linearProject && paramI(1) == 0;
        return {srgb, linearProject, paramI(2), srgb && paramB(5)};
    }
    bool roiSourceSize(const EvalContext& ctx, int& w, int& h) const override;
    // Color Space and Embedded Profile are for ordinary images (the profile only with sRGB);
    // Highlight Reconstruction and the exposure only for RAW.
    bool paramHidden(int i) const override {
        const bool isRaw = raw::isRawPath(paramS(0));
        if (i == 1) return isRaw;
        if (i == 5) return isRaw || paramI(1) != 0;
        return i >= 2 && !isRaw;
    }
};

class OutputNode : public Node {
public:
    NODELAB_NODE({"io.output", "Output", "Input / Output",
                  {{"Image", PinType::Image}},
                  {},
                  {}})
    void evaluate(EvalContext&, const std::vector<Value>&, std::vector<Value>&) override {}
    int roiPadding(const EvalContext&) const override { return 0; }
};

class NumberNode : public Node {
public:
    NODELAB_NODE({"io.number", "Number", "Input / Output",
                  {},
                  {{"Value", PinType::Number}},
                  {ParamDesc::Float("Value", 1.0f, -10.0f, 10.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>&, std::vector<Value>& out) override {
        out[0] = Value(paramF(0));
    }
};
