#include "core/ColorMath.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

class MixNode : public Node {
public:
    NODELAB_NODE({"math.mix", "Mix", "Mix",
                  {{"A", PinType::Image}, {"B", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 0.5f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        resolveSize(in, ctx, w, h);
        ImagePtr a = toImage(in[0], w, h), b = toImage(in[1], w, h);
        if (!a && !b) return;
        if (!a) { out[0] = Value(b); return; }
        if (!b) { out[0] = Value(a); return; }
        ChannelPtr fac = channelOr(in[2], 0.5f);
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            ImageSampler sa{a.get(), w, h}, sb{b.get(), w, h};
            ChannelSampler sf = paramSampler(*this, 2, fac, w, h);
            for (int x = 0; x < w; ++x) {
                const float* pa = sa(x, y);
                const float* pb = sb(x, y);
                float f = sf(x, y);
                float* d = img->pixel(size_t(y) * w + x);
                for (int k = 0; k < 4; ++k) d[k] = clamp01(pa[k] + (pb[k] - pa[k]) * f);
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

// Blend modes as in Blender's Mix Color node. B is blended over A.
enum BlendMode {
    Mix, Darken, Multiply, ColorBurn, Lighten, Screen, ColorDodge, Add, Overlay, SoftLight, LinearLight,
    Difference, Exclusion, Subtract, Divide, Hue, Saturation, ColorMode, ValueMode
};

float blendChannel(int mode, float a, float b) {
    switch (mode) {
        case Darken: return std::min(a, b);
        case Multiply: return a * b;
        case ColorBurn: return b <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - a) / b);
        case Lighten: return std::max(a, b);
        case Screen: return 1.0f - (1.0f - a) * (1.0f - b);
        case ColorDodge: return b >= 1.0f ? 1.0f : std::min(1.0f, a / (1.0f - b));
        case Add: return a + b;
        case Overlay: return a < 0.5f ? 2.0f * a * b : 1.0f - 2.0f * (1.0f - a) * (1.0f - b);
        case SoftLight: return (1.0f - 2.0f * b) * a * a + 2.0f * b * a;
        case LinearLight: return a + 2.0f * b - 1.0f;
        case Difference: return std::fabs(a - b);
        case Exclusion: return a + b - 2.0f * a * b;
        case Subtract: return a - b;
        case Divide: return b <= 1e-6f ? a : a / b;
        default: return b;
    }
}

void blendPixel(int mode, const float* a, const float* b, float* out) {
    using namespace colormath;
    if (mode >= Hue) {
        // HSV component swaps
        float ha, sa, va, hb, sb, vb;
        rgbToHsv(clamp01(a[0]), clamp01(a[1]), clamp01(a[2]), ha, sa, va);
        rgbToHsv(clamp01(b[0]), clamp01(b[1]), clamp01(b[2]), hb, sb, vb);
        switch (mode) {
            case Hue: hsvToRgb(hb, sa, va, out[0], out[1], out[2]); break;
            case Saturation: hsvToRgb(ha, sb, va, out[0], out[1], out[2]); break;
            case ColorMode: hsvToRgb(hb, sb, va, out[0], out[1], out[2]); break;
            default: hsvToRgb(ha, sa, vb, out[0], out[1], out[2]); break;
        }
        return;
    }
    for (int k = 0; k < 3; ++k) out[k] = blendChannel(mode, a[k], b[k]);
}

class BlendNode : public Node {
public:
    NODELAB_NODE({"math.blend", "Blend", "Mix",
                  {{"A", PinType::Image}, {"B", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Enum("Mode", Multiply,
                                   {"Mix", "Darken", "Multiply", "Color Burn", "Lighten", "Screen", "Color Dodge", "Add",
                                    "Overlay", "Soft Light", "Linear Light", "Difference", "Exclusion", "Subtract",
                                    "Divide", "Hue", "Saturation", "Color", "Value"}),
                   ParamDesc::Bool("Clamp", true)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        resolveSize(in, ctx, w, h);
        ImagePtr a = toImage(in[0], w, h), b = toImage(in[1], w, h);
        if (!a || !b) {
            if (a || b) out[0] = Value(a ? a : b);
            return;
        }
        const int mode = paramI(1);
        const bool clampOut = paramB(2);
        ChannelPtr fac = channelOr(in[2], 1.0f);
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            ImageSampler sa{a.get(), w, h}, sb{b.get(), w, h};
            ChannelSampler sf = paramSampler(*this, 2, fac, w, h);
            float blended[3];
            for (int x = 0; x < w; ++x) {
                const float* pa = sa(x, y);
                const float* pb = sb(x, y);
                float f = sf(x, y) * pb[3];  // B's alpha limits its influence
                blendPixel(mode, pa, pb, blended);
                float* d = img->pixel(size_t(y) * w + x);
                for (int k = 0; k < 3; ++k) {
                    d[k] = pa[k] + (blended[k] - pa[k]) * f;
                    if (clampOut) d[k] = clamp01(d[k]);
                }
                d[3] = pa[3];
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

}  // namespace

void registerMathNodes(NodeRegistry& r) {
    r.add<MixNode>();
    r.add<BlendNode>();
}
