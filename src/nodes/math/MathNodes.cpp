#include "core/ColorMath.h"
#include "gpu/PointOp.h"
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
                for (int k = 0; k < 3; ++k) d[k] = clampColor(ctx.linear(), pa[k] + (pb[k] - pa[k]) * f);
                d[3] = clamp01(pa[3] + (pb[3] - pa[3]) * f);
            }
        });
        out[0] = Value(ImagePtr(img));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        if (in[0].empty() && in[1].empty()) return;
        gpu::PointOp op;
        resolveSize(in, ctx, op.w, op.h);
        op.body = R"(
    if (!has0) { out0 = img1(p); return; }
    if (!has1) { out0 = img0(p); return; }
    vec4 a = img0(p), b = img1(p);
    float f = par2(p);
    out0 = vec4(clampColor(uLinear, a.rgb + (b.rgb - a.rgb) * f), clamp01(a.a + (b.a - a.a) * f));)";
        gpu::runPoint(ctx, *this, op, in, out);
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

void blendPixel(int mode, const float* a, const float* b, float* out, bool lin) {
    using namespace colormath;
    if (mode >= Hue) {
        // HSV component swaps
        float ha, sa, va, hb, sb, vb;
        rgbToHsv(clampColor(lin, a[0]), clampColor(lin, a[1]), clampColor(lin, a[2]), ha, sa, va);
        rgbToHsv(clampColor(lin, b[0]), clampColor(lin, b[1]), clampColor(lin, b[2]), hb, sb, vb);
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
                blendPixel(mode, pa, pb, blended, ctx.linear());
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

    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        if (in[0].empty() && in[1].empty()) return;
        gpu::PointOp op;
        resolveSize(in, ctx, op.w, op.h);
        // Mode and Clamp are compiled in, so each mode's shader holds only its own maths.
        op.functions = "const int MODE = " + std::to_string(paramI(1)) + ";\nconst bool CLAMP_OUT = " +
                       (paramB(2) ? "true" : "false") + ";\n" + R"(
float blendChannel(float a, float b) {
    switch (MODE) {
        case 1: return min(a, b);
        case 2: return a * b;
        case 3: return b <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - a) / b);
        case 4: return max(a, b);
        case 5: return 1.0 - (1.0 - a) * (1.0 - b);
        case 6: return b >= 1.0 ? 1.0 : min(1.0, a / (1.0 - b));
        case 7: return a + b;
        case 8: return a < 0.5 ? 2.0 * a * b : 1.0 - 2.0 * (1.0 - a) * (1.0 - b);
        case 9: return (1.0 - 2.0 * b) * a * a + 2.0 * b * a;
        case 10: return a + 2.0 * b - 1.0;
        case 11: return abs(a - b);
        case 12: return a + b - 2.0 * a * b;
        case 13: return a - b;
        case 14: return b <= 1e-6 ? a : a / b;
        default: return b;
    }
}
vec3 blendPixel(vec3 a, vec3 b) {
    if (MODE >= 15) {  // HSV component swaps
        vec3 ha = rgbToHsv(clampColor(uLinear, a)), hb = rgbToHsv(clampColor(uLinear, b));
        if (MODE == 15) return hsvToRgb(vec3(hb.x, ha.y, ha.z));
        if (MODE == 16) return hsvToRgb(vec3(ha.x, hb.y, ha.z));
        if (MODE == 17) return hsvToRgb(vec3(hb.x, hb.y, ha.z));
        return hsvToRgb(vec3(ha.x, ha.y, hb.z));
    }
    return vec3(blendChannel(a.r, b.r), blendChannel(a.g, b.g), blendChannel(a.b, b.b));
}
)";
        op.body = R"(
    if (!has0) { out0 = img1(p); return; }
    if (!has1) { out0 = img0(p); return; }
    vec4 a = img0(p), b = img1(p);
    float f = par2(p) * b.a;  // B's alpha limits its influence
    vec3 d = a.rgb + (blendPixel(a.rgb, b.rgb) - a.rgb) * f;
    out0 = vec4(CLAMP_OUT ? clamp01(d) : d, a.a);)";
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

}  // namespace

void registerMathNodes(NodeRegistry& r) {
    r.add<MixNode>();
    r.add<BlendNode>();
}
