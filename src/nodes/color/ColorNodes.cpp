#include "nodes/NodeUtil.h"

using namespace nodeutil;

// Color adjustment nodes produce displayable color, so their outputs are clamped to 0..1, and
// channels driving a parameter are clamped to that parameter's range.

namespace {

class SplitRGBNode : public Node {
public:
    NODELAB_NODE({"color.split_rgb", "Split RGB", "Color",
                  {{"Image", PinType::Image}},
                  {{"R", PinType::Channel}, {"G", PinType::Channel}, {"B", PinType::Channel}, {"A", PinType::Channel}},
                  {}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr img = toImage(in[0], 0, 0);
        if (!img) return;
        std::shared_ptr<Channel> ch[4];
        for (auto& c : ch) c = std::make_shared<Channel>(Channel::makeSized(img->w, img->h));
        parallelFor(img->h, [&](int y) {
            for (int x = 0; x < img->w; ++x) {
                size_t i = size_t(y) * img->w + x;
                const float* p = img->pixel(i);
                for (int k = 0; k < 4; ++k) ch[k]->data[i] = p[k];
            }
        });
        for (int k = 0; k < 4; ++k) out[k] = Value(ChannelPtr(ch[k]));
    }
};

class CombineRGBNode : public Node {
public:
    NODELAB_NODE({"color.combine_rgb", "Combine RGB", "Color",
                  {{"R", PinType::Channel, 0}, {"G", PinType::Channel, 1}, {"B", PinType::Channel, 2}, {"A", PinType::Channel, 3}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("R", 0.0f, 0.0f, 1.0f), ParamDesc::Float("G", 0.0f, 0.0f, 1.0f),
                   ParamDesc::Float("B", 0.0f, 0.0f, 1.0f), ParamDesc::Float("A", 1.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        resolveSize(in, ctx, w, h);
        ChannelPtr c[4];
        for (int k = 0; k < 4; ++k) c[k] = channelOr(in[k], k == 3 ? 1.0f : 0.0f);
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            ChannelSampler s[4] = {paramSampler(*this, 0, c[0], w, h), paramSampler(*this, 1, c[1], w, h),
                                   paramSampler(*this, 2, c[2], w, h), paramSampler(*this, 3, c[3], w, h)};
            for (int x = 0; x < w; ++x) {
                float* d = img->pixel(size_t(y) * w + x);
                for (int k = 0; k < 4; ++k) d[k] = s[k](x, y);
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

class BrightnessContrastNode : public Node {
public:
    NODELAB_NODE({"color.brightness_contrast", "Brightness / Contrast", "Color",
                  {{"Image", PinType::Image}, {"Brightness", PinType::Channel, 0}, {"Contrast", PinType::Channel, 1}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Brightness", 0.0f, -1.0f, 1.0f), ParamDesc::Float("Contrast", 0.0f, -1.0f, 1.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr br = channelOr(in[1], 0.0f), co = channelOr(in[2], 0.0f);
        const int w = src->w, h = src->h;
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            ChannelSampler sb = paramSampler(*this, 1, br, w, h), sc = paramSampler(*this, 2, co, w, h);
            for (int x = 0; x < w; ++x) {
                size_t i = size_t(y) * w + x;
                const float* s = src->pixel(i);
                float* d = img->pixel(i);
                // Contrast in -1..1 maps to a slope of 0..inf around mid-gray.
                float c = std::min(sc(x, y), 0.999f);
                float slope = (1.0f + c) / (1.0f - c);
                float b = sb(x, y);
                for (int k = 0; k < 3; ++k) d[k] = clamp01((s[k] - 0.5f) * slope + 0.5f + b);
                d[3] = s[3];
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

class SaturationNode : public Node {
public:
    NODELAB_NODE({"color.saturation", "Saturation", "Color",
                  {{"Image", PinType::Image}, {"Amount", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Amount", 1.0f, 0.0f, 4.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr amt = channelOr(in[1], 1.0f);
        const int w = src->w, h = src->h;
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            ChannelSampler sa = paramSampler(*this, 1, amt, w, h);
            for (int x = 0; x < w; ++x) {
                size_t i = size_t(y) * w + x;
                const float* s = src->pixel(i);
                float* d = img->pixel(i);
                float a = sa(x, y);
                float l = luminance(s[0], s[1], s[2]);
                for (int k = 0; k < 3; ++k) d[k] = clamp01(l + (s[k] - l) * a);
                d[3] = s[3];
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

class InvertNode : public Node {
public:
    NODELAB_NODE({"color.invert", "Invert", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr fac = channelOr(in[1], 1.0f);
        const int w = src->w, h = src->h;
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            ChannelSampler sf = paramSampler(*this, 1, fac, w, h);
            for (int x = 0; x < w; ++x) {
                size_t i = size_t(y) * w + x;
                const float* s = src->pixel(i);
                float* d = img->pixel(i);
                float f = sf(x, y);
                for (int k = 0; k < 3; ++k) {
                    float v = clamp01(s[k]);
                    d[k] = v + ((1.0f - v) - v) * f;
                }
                d[3] = s[3];
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

}  // namespace

void registerColorNodes(NodeRegistry& r) {
    r.add<SplitRGBNode>();
    r.add<CombineRGBNode>();
    r.add<BrightnessContrastNode>();
    r.add<SaturationNode>();
    r.add<InvertNode>();
}
