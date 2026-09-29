#pragma once
#include "nodes/NodeUtil.h"

class ImageInputNode : public Node {
public:
    NODELAB_NODE({"io.image_input", "Image Input", "Input / Output",
                  {},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Path("File")}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;
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
