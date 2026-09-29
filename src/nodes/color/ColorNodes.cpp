#include "core/ColorMath.h"
#include "core/Curve.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;
using namespace colormath;

// Color adjustment nodes produce displayable color, so their outputs are clamped to 0..1, and
// channels driving a parameter are clamped to that parameter's range.

namespace {

// ---------------------------------------------------------------- split / combine

// Shared implementation for split nodes: fn(r, g, b, out[3]).
template <typename Fn>
void splitImage(const Value& in, std::vector<Value>& out, int count, Fn&& fn) {
    ImagePtr img = toImage(in, 0, 0);
    if (!img) return;
    std::vector<std::shared_ptr<Channel>> ch(count);
    for (auto& c : ch) c = std::make_shared<Channel>(Channel::makeSized(img->w, img->h));
    parallelFor(img->h, [&](int y) {
        float o[4];
        for (int x = 0; x < img->w; ++x) {
            size_t i = size_t(y) * img->w + x;
            const float* p = img->pixel(i);
            fn(p, o);
            for (int k = 0; k < count; ++k) ch[k]->data[i] = o[k];
        }
    });
    for (int k = 0; k < count; ++k) out[k] = Value(ChannelPtr(ch[k]));
}

// Shared implementation for combine nodes: fn(c0, c1, c2, rgbOut[3]); input 3 is alpha.
template <typename Fn>
void combineImage(const Node& node, EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out,
                  const float defs[4], bool clampOut, Fn&& fn) {
    int w, h;
    resolveSize(in, ctx, w, h);
    ChannelPtr c[4];
    for (int k = 0; k < 4; ++k) c[k] = channelOr(in[k], defs[k]);
    auto img = std::make_shared<Image>(w, h);
    parallelFor(h, [&](int y) {
        ChannelSampler s[4] = {paramSampler(node, 0, c[0], w, h), paramSampler(node, 1, c[1], w, h),
                               paramSampler(node, 2, c[2], w, h), paramSampler(node, 3, c[3], w, h)};
        for (int x = 0; x < w; ++x) {
            float* d = img->pixel(size_t(y) * w + x);
            fn(s[0](x, y), s[1](x, y), s[2](x, y), d);
            if (clampOut)
                for (int k = 0; k < 3; ++k) d[k] = clamp01(d[k]);
            d[3] = clamp01(s[3](x, y));
        }
    });
    out[0] = Value(ImagePtr(img));
}

class SplitRGBNode : public Node {
public:
    NODELAB_NODE({"color.split_rgb", "Split RGB", "Color",
                  {{"Image", PinType::Image}},
                  {{"R", PinType::Channel}, {"G", PinType::Channel}, {"B", PinType::Channel}, {"A", PinType::Channel}},
                  {}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        splitImage(in[0], out, 4, [](const float* p, float* o) {
            for (int k = 0; k < 4; ++k) o[k] = p[k];
        });
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
        const float defs[4] = {0, 0, 0, 1};
        combineImage(*this, ctx, in, out, defs, true, [](float r, float g, float b, float* d) {
            d[0] = r, d[1] = g, d[2] = b;
        });
    }
};

class SplitHSVNode : public Node {
public:
    NODELAB_NODE({"color.split_hsv", "Split HSV", "Color",
                  {{"Image", PinType::Image}},
                  {{"H", PinType::Channel}, {"S", PinType::Channel}, {"V", PinType::Channel}, {"A", PinType::Channel}},
                  {}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        splitImage(in[0], out, 4, [](const float* p, float* o) {
            rgbToHsv(clamp01(p[0]), clamp01(p[1]), clamp01(p[2]), o[0], o[1], o[2]);
            o[3] = p[3];
        });
    }
};

class CombineHSVNode : public Node {
public:
    NODELAB_NODE({"color.combine_hsv", "Combine HSV", "Color",
                  {{"H", PinType::Channel, 0}, {"S", PinType::Channel, 1}, {"V", PinType::Channel, 2}, {"A", PinType::Channel, 3}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::FloatFree("H", 0.0f, 0.0f, 1.0f), ParamDesc::Float("S", 0.0f, 0.0f, 1.0f),
                   ParamDesc::Float("V", 0.0f, 0.0f, 1.0f), ParamDesc::Float("A", 1.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const float defs[4] = {0, 0, 0, 1};
        combineImage(*this, ctx, in, out, defs, true, [](float h, float s, float v, float* d) {
            hsvToRgb(h, s, v, d[0], d[1], d[2]);  // hue wraps, so it is left unclamped
        });
    }
};

class SplitLabNode : public Node {
public:
    NODELAB_NODE({"color.split_lab", "Split Lab", "Color",
                  {{"Image", PinType::Image}},
                  {{"L", PinType::Channel}, {"a", PinType::Channel}, {"b", PinType::Channel}, {"A", PinType::Channel}},
                  {}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        splitImage(in[0], out, 4, [](const float* p, float* o) {
            rgbToLab(clamp01(p[0]), clamp01(p[1]), clamp01(p[2]), o[0], o[1], o[2]);
            o[3] = p[3];
        });
    }
};

class CombineLabNode : public Node {
public:
    NODELAB_NODE({"color.combine_lab", "Combine Lab", "Color",
                  {{"L", PinType::Channel, 0}, {"a", PinType::Channel, 1}, {"b", PinType::Channel, 2}, {"A", PinType::Channel, 3}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("L", 0.5f, 0.0f, 1.0f), ParamDesc::Float("a", 0.0f, -1.0f, 1.0f),
                   ParamDesc::Float("b", 0.0f, -1.0f, 1.0f), ParamDesc::Float("A", 1.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const float defs[4] = {0.5f, 0, 0, 1};
        combineImage(*this, ctx, in, out, defs, true, [](float l, float a, float b, float* d) {
            labToRgb(l, a, b, d[0], d[1], d[2]);
        });
    }
};

class LuminanceNode : public Node {
public:
    NODELAB_NODE({"color.luminance", "Luminance", "Color",
                  {{"Image", PinType::Image}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::Enum("Method", 0, {"Rec.709 luma", "Average", "Max (HSV value)", "Lab lightness"})}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr img = toImage(in[0], 0, 0);
        if (!img) return;
        const int method = paramI(0);
        out[0] = Value(ChannelPtr(makeChannel(img->w, img->h, [&](int x, int y) {
            const float* p = img->pixel(size_t(y) * img->w + x);
            switch (method) {
                case 1: return (p[0] + p[1] + p[2]) / 3.0f;
                case 2: return std::max({p[0], p[1], p[2]});
                case 3: {
                    float L, a, b;
                    rgbToLab(clamp01(p[0]), clamp01(p[1]), clamp01(p[2]), L, a, b);
                    return L;
                }
                default: return luminance(p[0], p[1], p[2]);
            }
        })));
    }
};

// ---------------------------------------------------------------- adjustments

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
        ChannelSampler sb = paramSampler(*this, 1, br, src->w, src->h), sc = paramSampler(*this, 2, co, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            // Contrast in -1..1 maps to a slope of 0..inf around mid-gray.
            float c = std::min(sc(x, y), 0.999f);
            float slope = (1.0f + c) / (1.0f - c);
            float b = sb(x, y);
            for (int k = 0; k < 3; ++k) d[k] = clamp01((s[k] - 0.5f) * slope + 0.5f + b);
            d[3] = s[3];
        })));
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
        ChannelSampler sa = paramSampler(*this, 1, amt, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float a = sa(x, y);
            float l = luminance(s[0], s[1], s[2]);
            for (int k = 0; k < 3; ++k) d[k] = clamp01(l + (s[k] - l) * a);
            d[3] = s[3];
        })));
    }
};

class HueShiftNode : public Node {
public:
    NODELAB_NODE({"color.hue_shift", "Hue Shift", "Color",
                  {{"Image", PinType::Image}, {"Degrees", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Degrees", 0.0f, -180.0f, 180.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr deg = channelOr(in[1], 0.0f);
        ChannelSampler sd = paramSampler(*this, 1, deg, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float h, sat, v;
            rgbToHsv(clamp01(s[0]), clamp01(s[1]), clamp01(s[2]), h, sat, v);
            hsvToRgb(h + sd(x, y) / 360.0f, sat, v, d[0], d[1], d[2]);
            d[3] = s[3];
        })));
    }
};

class GammaNode : public Node {
public:
    NODELAB_NODE({"color.gamma", "Gamma", "Color",
                  {{"Image", PinType::Image}, {"Gamma", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Gamma", 1.0f, 0.1f, 5.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr gm = channelOr(in[1], 1.0f);
        ChannelSampler sg = paramSampler(*this, 1, gm, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float inv = 1.0f / sg(x, y);  // > 1 brightens midtones, like the Levels gamma
            for (int k = 0; k < 3; ++k) d[k] = std::pow(clamp01(s[k]), inv);
            d[3] = s[3];
        })));
    }
};

class ExposureNode : public Node {
public:
    NODELAB_NODE({"color.exposure", "Exposure", "Color",
                  {{"Image", PinType::Image}, {"Stops", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Stops", 0.0f, -5.0f, 5.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr st = channelOr(in[1], 0.0f);
        ChannelSampler ss = paramSampler(*this, 1, st, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float m = std::exp2(ss(x, y));  // scale in linear light, like a camera exposure change
            for (int k = 0; k < 3; ++k) d[k] = clamp01(linearToSrgb(srgbToLinear(clamp01(s[k])) * m));
            d[3] = s[3];
        })));
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
        ChannelSampler sf = paramSampler(*this, 1, fac, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float f = sf(x, y);
            for (int k = 0; k < 3; ++k) {
                float v = clamp01(s[k]);
                d[k] = v + ((1.0f - v) - v) * f;
            }
            d[3] = s[3];
        })));
    }
};

class LevelsNode : public Node {
public:
    NODELAB_NODE({"color.levels", "Levels", "Color",
                  {{"Image", PinType::Image}, {"In Black", PinType::Channel, 0}, {"In White", PinType::Channel, 1},
                   {"Gamma", PinType::Channel, 2}, {"Out Black", PinType::Channel, 3}, {"Out White", PinType::Channel, 4}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("In Black", 0.0f, 0.0f, 1.0f), ParamDesc::Float("In White", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Gamma", 1.0f, 0.1f, 5.0f), ParamDesc::Float("Out Black", 0.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Out White", 1.0f, 0.0f, 1.0f), ParamDesc::Enum("Channel", 0, {"RGB", "Red", "Green", "Blue"})}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int w = src->w, h = src->h;
        ChannelPtr c[5];
        const float defs[5] = {0, 1, 1, 0, 1};
        for (int k = 0; k < 5; ++k) c[k] = channelOr(in[k + 1], defs[k]);
        ChannelSampler s[5] = {paramSampler(*this, 1, c[0], w, h), paramSampler(*this, 2, c[1], w, h),
                               paramSampler(*this, 3, c[2], w, h), paramSampler(*this, 4, c[3], w, h),
                               paramSampler(*this, 5, c[4], w, h)};
        const int only = paramI(5) - 1;  // -1 = all channels
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* p, float* d) {
            float ib = s[0](x, y), iw = s[1](x, y), gm = s[2](x, y), ob = s[3](x, y), ow = s[4](x, y);
            for (int k = 0; k < 3; ++k) {
                if (only >= 0 && k != only) {
                    d[k] = p[k];
                    continue;
                }
                float v = clamp01((p[k] - ib) / std::max(iw - ib, 1e-4f));
                v = std::pow(v, 1.0f / gm);
                d[k] = clamp01(ob + v * (ow - ob));
            }
            d[3] = p[3];
        })));
    }
};

class CurvesNode : public Node {
public:
    NODELAB_NODE({"color.curves", "Curves", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f), ParamDesc::Curve("Curves")}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const nlohmann::json& cj = params[1];
        auto lut = [&](const char* key) {
            return curveLut(curveFromJson(cj.is_object() && cj.contains(key) ? cj[key] : nlohmann::json()));
        };
        const std::vector<float> master = lut("master"), ch[3] = {lut("r"), lut("g"), lut("b")};
        ChannelPtr fac = channelOr(in[1], 1.0f);
        ChannelSampler sf = paramSampler(*this, 1, fac, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float f = sf(x, y);
            for (int k = 0; k < 3; ++k) {
                // Per-channel curve first, then the master curve (same order as Blender / Photoshop).
                float v = lutLookup(master, lutLookup(ch[k], clamp01(s[k])));
                d[k] = s[k] + (v - s[k]) * f;
            }
            d[3] = s[3];
        })));
    }
};

}  // namespace

void registerColorNodes(NodeRegistry& r) {
    r.add<BrightnessContrastNode>();
    r.add<SaturationNode>();
    r.add<HueShiftNode>();
    r.add<ExposureNode>();
    r.add<GammaNode>();
    r.add<LevelsNode>();
    r.add<CurvesNode>();
    r.add<InvertNode>();
    r.add<SplitRGBNode>();
    r.add<CombineRGBNode>();
    r.add<SplitHSVNode>();
    r.add<CombineHSVNode>();
    r.add<SplitLabNode>();
    r.add<CombineLabNode>();
    r.add<LuminanceNode>();
}
