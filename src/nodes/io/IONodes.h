#pragma once
#include "io/ImageCache.h"
#include "nodes/NodeUtil.h"

class ImageInputNode : public Node {
public:
    NODELAB_NODE({"io.image_input", "Image Input", "Input / Output",
                  {},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Path("File"), ParamDesc::Enum("Color Space", 0, {"sRGB", "Linear Rec.709", "Non-Color"})}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;

    // How the file's values are read. Legacy projects take them as they are, whatever the
    // Color Space; scene-linear ones decode sRGB files to linear light.
    ImageCache::Decode decode(bool linearProject) const {
        return linearProject && paramI(1) == 0 ? ImageCache::Decode::SrgbToLinear : ImageCache::Decode::AsIs;
    }
};

class OutputNode : public Node {
public:
    NODELAB_NODE({"io.output", "Output", "Input / Output",
                  {{"Image", PinType::Image}},
                  {},
                  {}})
    void evaluate(EvalContext&, const std::vector<Value>&, std::vector<Value>&) override {}
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
