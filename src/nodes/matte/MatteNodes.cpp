// Matte nodes: shape masks and keyers. Keyers output a Matte channel (1 = keep) and the keyed
// image (color * matte, alpha = matte).
#include "nodes/matte/MatteNodes.h"

#include <cmath>
#include <cstring>

#include "core/ColorMath.h"
#include "gpu/PointOp.h"
#include "nodes/ImageOps.h"

using namespace nodeutil;
using namespace colormath;

namespace {

constexpr float kPi = 3.14159265f;

float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0f : 1.0f;
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Emits Matte + keyed Image from a per-pixel matte function.
template <typename Fn>
void keyOutputs(const Image& src, bool invert, std::vector<Value>& out, Fn&& matteOf) {
    auto matte = std::make_shared<Channel>(Channel::makeSized(src.w, src.h));
    auto keyed = mapImage(src, [&](int x, int y, const float* s, float* d) {
        float m = clamp01(matteOf(s, x, y));
        if (invert) m = 1.0f - m;
        matte->data[size_t(y) * src.w + x] = m;
        for (int k = 0; k < 3; ++k) d[k] = s[k] * m;
        d[3] = s[3] * m;
    });
    out[0] = Value(ChannelPtr(matte));
    out[1] = Value(ImagePtr(keyed));
}

// The GPU version of keyOutputs. `matte` sets `float m` from `s` (the pixel) and, for keyers with
// a Key input (pin 1), `k`: the Key input's pixel (clamped to its edges, as KeySource::at) or
// the Key Color param in P[0..2].
void gpuKey(EvalContext& ctx, const Node& node, const std::vector<Value>& in, std::vector<Value>& out,
            const std::string& matte, std::vector<float> params, bool invert, bool key, const std::string& functions = "") {
    gpu::PointOp g;
    g.functions = functions;
    g.body = "    vec4 s = img0(p);\n";
    if (key) {
        g.body += "    vec3 k = has1 ? fetch1(p).rgb : vec3(P[0], P[1], P[2]);\n";
        g.gather = {1};
    }
    g.body += "    float m;\n" + matte + "    m = clamp01(m);\n" + (invert ? "    m = 1.0 - m;\n" : "") +
              "    out0 = m;\n    out1 = s * m;\n";
    g.params = std::move(params);
    gpu::runOver(ctx, node, g, in, out);
}

// Key color: the Key input's pixel if connected, otherwise the Color param.
struct KeySource {
    ImagePtr img;
    float color[3];
    const float* at(int x, int y) const {
        if (img && img->w > 0) return img->pixel(size_t(std::min(y, img->h - 1)) * img->w + std::min(x, img->w - 1));
        return color;
    }
};

// ---------------------------------------------------------------- shape masks

// Combines a mask value into the base mask (the shape masks' Operation).
float combineMask(int op, float b, float v) {
    switch (op) {
        case 1: return clamp01(b - v);                  // Subtract
        case 2: return clamp01(b * v);                  // Multiply
        case 3: return clamp01(std::max(b, 1.0f - v));  // Not: everything outside the shape
        default: return clamp01(std::max(b, v));        // Add
    }
}

// Size of a mask node's output: its Mask input's, else the working size (or region window).
void maskSize(const ChannelPtr& base, const EvalContext& ctx, int& w, int& h) {
    if (base && !base->constant) {
        w = base->w;
        h = base->h;
    } else {
        resolveSize({}, ctx, w, h);
    }
}

// GLSL versions of smoothstep (with its degenerate case) and combineMask.
const char* const kGlslMatte = R"(
float smoothstepC(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0 : 1.0;
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
float combineMask(int op, float b, float v) {
    if (op == 1) return clamp01(b - v);
    if (op == 2) return clamp01(b * v);
    if (op == 3) return clamp01(max(b, 1.0 - v));
    return clamp01(max(b, v));
}
)";

// Masks are drawn per pixel from its position (in the full image), so a region draws its window.
class MaskBase : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }
    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; }

protected:
    // Output size on the GPU: maskSize's.
    static void gpuSize(const EvalContext& ctx, const std::vector<Value>& in, int& w, int& h) {
        if (!in[0].size(w, h)) resolveSize({}, ctx, w, h);
    }
    // The full image's size, which the shapes are relative to (a region's buffer is part of it).
    static void gpuFullSize(const EvalContext& ctx, const std::vector<Value>& in, int& w, int& h) {
        gpuSize(ctx, in, w, h);
        const PixelFrame fr = frameOf(ctx, w, h);
        w = fr.fullW, h = fr.fullH;
    }
    // The GPU version: body computes `float shape` at pixel position `xy` (full-image pixels).
    void runGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out, const std::string& body,
                std::vector<float> params, int op) {
        gpu::PointOp g;
        gpuSize(ctx, in, g.w, g.h);
        g.functions = kGlslMatte;
        g.body = "    vec2 xy = vec2(p + uOrigin) + 0.5;\n" + body +
                 "    out0 = combineMask(" + std::to_string(op) + ", has0 ? ch0(p) : 0.0, par1(p) * shape);\n";
        g.params = std::move(params);
        g.defaults = {NAN, 1.0f};
        gpu::runPoint(ctx, *this, g, in, out);
    }
};

class ShapeMaskNode : public MaskBase {
protected:
    // pinned param indices: 0 X, 1 Y, 2 Width, 3 Height, 4 Rotation, 5 Feather, 6 Value, 7 Operation
    void run(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out, bool ellipse) {
        ChannelPtr base = toChannel(in[0]);
        int bw, bh;
        maskSize(base, ctx, bw, bh);
        const PixelFrame fr = frameOf(ctx, bw, bh);
        const int w = fr.fullW, h = fr.fullH;
        if (!base) base = std::make_shared<Channel>(Channel::makeConstant(0.0f));
        ChannelPtr val = channelOr(in[1], 1.0f);
        ChannelSampler sb{base.get(), bw, bh}, sv = paramSampler(*this, 1, val, bw, bh);
        const float cx = paramF(0) * w, cy = paramF(1) * h;
        const float hw = std::max(paramF(2) * w * 0.5f, 1e-3f), hh = std::max(paramF(3) * h * 0.5f, 1e-3f);
        const float a = -paramF(4) * kPi / 180.0f, ca = std::cos(a), sa = std::sin(a);
        const float feather = paramF(5);
        const int op = paramI(7);
        out[0] = Value(ChannelPtr(makeChannel(bw, bh, [&](int bx, int by) {
            const int x = bx + fr.x0, y = by + fr.y0;
            float px = x + 0.5f - cx, py = y + 0.5f - cy;
            float rx = px * ca - py * sa, ry = px * sa + py * ca;
            // Normalized "radius": <1 inside. Box uses the max norm, ellipse the Euclidean one.
            float q = ellipse ? std::hypot(rx / hw, ry / hh) : std::max(std::fabs(rx) / hw, std::fabs(ry) / hh);
            float shape = 1.0f - smoothstep(1.0f - feather, 1.0f + 1e-4f, q);
            return combineMask(op, sb(bx, by), sv(bx, by) * shape);
        })));
    }
    void runGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out, bool ellipse) {
        int w, h;
        gpuFullSize(ctx, in, w, h);
        const float hw = std::max(paramF(2) * w * 0.5f, 1e-3f), hh = std::max(paramF(3) * h * 0.5f, 1e-3f);
        const float a = -paramF(4) * kPi / 180.0f;
        std::string body = R"(
    vec2 d = xy - vec2(P[0], P[1]);
    float rx = d.x * P[4] - d.y * P[5], ry = d.x * P[5] + d.y * P[4];
)";
        body += ellipse ? "    float q = length(vec2(rx / P[2], ry / P[3]));\n"
                        : "    float q = max(abs(rx) / P[2], abs(ry) / P[3]);\n";
        body += "    float shape = 1.0 - smoothstepC(1.0 - P[6], 1.0 + 1e-4, q);\n";
        MaskBase::runGpu(ctx, in, out, body, {paramF(0) * w, paramF(1) * h, hw, hh, std::cos(a), std::sin(a), paramF(5)},
                         paramI(7));
    }
};

#define SHAPE_PARAMS_D(wd, ht, fe)                                                                          \
    {ParamDesc::Float("X", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Y", 0.5f, 0.0f, 1.0f),                    \
     ParamDesc::Float("Width", wd, 0.0f, 2.0f), ParamDesc::Float("Height", ht, 0.0f, 2.0f),               \
     ParamDesc::Float("Rotation", 0.0f, -180.0f, 180.0f), ParamDesc::Float("Feather", fe, 0.0f, 1.0f),    \
     ParamDesc::Float("Value", 1.0f, 0.0f, 1.0f), ParamDesc::Enum("Operation", 0, {"Add", "Subtract", "Multiply", "Not"})}
#define SHAPE_PARAMS SHAPE_PARAMS_D(0.4f, 0.3f, 0.1f)

class BoxMaskNode : public ShapeMaskNode {
public:
    NODELAB_NODE({"matte.box_mask", "Box Mask", "Matte",
                  {{"Mask", PinType::Channel}, {"Value", PinType::Channel, 6}},
                  {{"Mask", PinType::Channel}},
                  SHAPE_PARAMS})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { run(ctx, in, out, false); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        runGpu(ctx, in, out, false);
    }
};

class EllipseMaskNode : public ShapeMaskNode {
public:
    NODELAB_NODE({"matte.ellipse_mask", "Ellipse Mask", "Matte",
                  {{"Mask", PinType::Channel}, {"Value", PinType::Channel, 6}},
                  {{"Mask", PinType::Channel}},
                  SHAPE_PARAMS})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { run(ctx, in, out, true); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        runGpu(ctx, in, out, true);
    }
};

// Lightroom's Radial Gradient: an ellipse mask with a wide feather. Operation "Not" is its Invert.
class RadialGradientNode : public ShapeMaskNode {
public:
    NODELAB_NODE({"matte.radial_gradient", "Radial Gradient", "Matte",
                  {{"Mask", PinType::Channel}, {"Value", PinType::Channel, 6}},
                  {{"Mask", PinType::Channel}},
                  SHAPE_PARAMS_D(0.6f, 0.6f, 0.5f)})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { run(ctx, in, out, true); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        runGpu(ctx, in, out, true);
    }
};

// Lightroom's Linear Gradient: full strength before Start, fading to nothing at End.
class LinearGradientNode : public MaskBase {
public:
    NODELAB_NODE({"matte.linear_gradient", "Linear Gradient", "Matte",
                  {{"Mask", PinType::Channel}, {"Value", PinType::Channel, 4}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::FloatFree("Start X", 0.5f, 0.0f, 1.0f), ParamDesc::FloatFree("Start Y", 0.2f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("End X", 0.5f, 0.0f, 1.0f), ParamDesc::FloatFree("End Y", 0.6f, 0.0f, 1.0f),
                   ParamDesc::Float("Value", 1.0f, 0.0f, 1.0f), ParamDesc::Enum("Operation", 0, {"Add", "Subtract", "Multiply", "Not"})}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ChannelPtr base = toChannel(in[0]);
        int bw, bh;
        maskSize(base, ctx, bw, bh);
        const PixelFrame fr = frameOf(ctx, bw, bh);
        const int w = fr.fullW, h = fr.fullH;
        if (!base) base = std::make_shared<Channel>(Channel::makeConstant(0.0f));
        ChannelPtr val = channelOr(in[1], 1.0f);
        ChannelSampler sb{base.get(), bw, bh}, sv = paramSampler(*this, 1, val, bw, bh);
        // In pixels, so the fade stays perpendicular to the Start-End line on non-square images.
        const float x0 = paramF(0) * w, y0 = paramF(1) * h;
        const float dx = paramF(2) * w - x0, dy = paramF(3) * h - y0;
        const float len2 = std::max(dx * dx + dy * dy, 1e-6f);
        const int op = paramI(5);
        out[0] = Value(ChannelPtr(makeChannel(bw, bh, [&](int bx, int by) {
            const int x = bx + fr.x0, y = by + fr.y0;
            float t = ((x + 0.5f - x0) * dx + (y + 0.5f - y0) * dy) / len2;
            return combineMask(op, sb(bx, by), sv(bx, by) * (1.0f - smoothstep(0.0f, 1.0f, t)));
        })));
    }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        gpuFullSize(ctx, in, w, h);
        const float x0 = paramF(0) * w, y0 = paramF(1) * h;
        const float dx = paramF(2) * w - x0, dy = paramF(3) * h - y0;
        runGpu(ctx, in, out,
               R"(    float t = dot(xy - vec2(P[0], P[1]), vec2(P[2], P[3])) / P[4];
    float shape = 1.0 - smoothstepC(0.0, 1.0, t);
)",
               {x0, y0, dx, dy, std::max(dx * dx + dy * dy, 1e-6f)}, paramI(5));
    }
};

// ---------------------------------------------------------------- keyers

class ChannelKeyNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.channel_key", "Channel Key", "Matte",
                  {{"Image", PinType::Image}},
                  {{"Matte", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Enum("Channel", 1, {"Red", "Green", "Blue", "Hue", "Saturation", "Value", "Y (luma)", "Cb", "Cr"}),
                   ParamDesc::Float("Low", 0.3f, 0.0f, 1.0f), ParamDesc::Float("High", 0.7f, 0.0f, 1.0f),
                   ParamDesc::Bool("Invert", false)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int ch = paramI(0);
        const float lo = paramF(1), hi = paramF(2);
        // Pixels whose channel is at or above High are keyed out; at or below Low are kept.
        keyOutputs(*src, paramB(3), out, [&](const float* s, int, int) {
            float r = clamp01(s[0]), g = clamp01(s[1]), b = clamp01(s[2]), v;
            if (ch < 3) {
                v = s[ch];
            } else if (ch < 6) {
                float hsv[3];
                rgbToHsv(r, g, b, hsv[0], hsv[1], hsv[2]);
                v = hsv[ch - 3];
            } else {
                float ycc[3];
                rgbToYCbCr(r, g, b, ycc[0], ycc[1], ycc[2]);
                v = ycc[ch - 6];
            }
            return 1.0f - smoothstep(lo, std::max(hi, lo + 1e-4f), v);
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpuKey(ctx, *this, in, out, R"(
    vec3 c = clamp01(s.rgb);
    float v = CH < 3 ? s[min(CH, 2)] : (CH < 6 ? rgbToHsv(c)[clamp(CH - 3, 0, 2)] : rgbToYCbCr(c)[max(CH - 6, 0)]);
    m = 1.0 - smoothstepC(P[0], max(P[1], P[0] + 1e-4), v);
)",
               {paramF(1), paramF(2)}, paramB(3), false, std::string(kGlslMatte) + "const int CH = " + std::to_string(paramI(0)) + ";\n");
    }
};

class LuminanceKeyNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.luminance_key", "Luminance Key", "Matte",
                  {{"Image", PinType::Image}},
                  {{"Matte", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Float("Low", 0.2f, 0.0f, 1.0f), ParamDesc::Float("High", 0.8f, 0.0f, 1.0f),
                   ParamDesc::Bool("Keep Bright", true)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float lo = paramF(0), hi = std::max(paramF(1), paramF(0) + 1e-4f);
        keyOutputs(*src, !paramB(2), out, [&](const float* s, int, int) {
            return smoothstep(lo, hi, luminance(s[0], s[1], s[2]));
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpuKey(ctx, *this, in, out, "    m = smoothstepC(P[0], P[1], luminance(s.rgb));\n",
               {paramF(0), std::max(paramF(1), paramF(0) + 1e-4f)}, !paramB(2), false, kGlslMatte);
    }
};

class DifferenceKeyNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.difference_key", "Difference Key", "Matte",
                  {{"Image", PinType::Image}, {"Key", PinType::Image}},
                  {{"Matte", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Color("Key Color", 0.1f, 0.8f, 0.1f), ParamDesc::Float("Tolerance", 0.1f, 0.0f, 1.0f),
                   ParamDesc::Float("Falloff", 0.1f, 0.0f, 1.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        KeySource key{toImage(in[1], src->w, src->h), {}};
        paramC(0, key.color);
        const float tol = paramF(1), fall = std::max(paramF(2), 1e-4f);
        // Largest per-channel difference from the key: similar pixels (small difference) are keyed out.
        keyOutputs(*src, false, out, [&](const float* s, int x, int y) {
            const float* k = key.at(x, y);
            float diff = std::max({std::fabs(s[0] - k[0]), std::fabs(s[1] - k[1]), std::fabs(s[2] - k[2])});
            return (diff - tol) / fall;
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        float c[3];
        paramC(0, c);
        gpuKey(ctx, *this, in, out, R"(
    vec3 d = abs(s.rgb - k);
    m = (max(d.r, max(d.g, d.b)) - P[3]) / P[4];
)",
               {c[0], c[1], c[2], paramF(1), std::max(paramF(2), 1e-4f)}, false, true);
    }
};

class DistanceKeyNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.distance_key", "Distance Key", "Matte",
                  {{"Image", PinType::Image}, {"Key", PinType::Image}},
                  {{"Matte", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Color("Key Color", 0.1f, 0.8f, 0.1f), ParamDesc::Float("Tolerance", 0.1f, 0.0f, 1.0f),
                   ParamDesc::Float("Falloff", 0.1f, 0.0f, 1.0f), ParamDesc::Enum("Space", 0, {"RGB", "YCbCr (ignore brightness)"})}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        KeySource key{toImage(in[1], src->w, src->h), {}};
        paramC(0, key.color);
        const float tol = paramF(1), fall = std::max(paramF(2), 1e-4f);
        const bool ycc = paramI(3) == 1;
        keyOutputs(*src, false, out, [&](const float* s, int x, int y) {
            const float* k = key.at(x, y);
            float d;
            if (ycc) {
                float a[3], b[3];
                rgbToYCbCr(s[0], s[1], s[2], a[0], a[1], a[2]);
                rgbToYCbCr(k[0], k[1], k[2], b[0], b[1], b[2]);
                d = std::hypot(a[1] - b[1], a[2] - b[2]) * 2.0f;
            } else {
                d = std::sqrt((s[0] - k[0]) * (s[0] - k[0]) + (s[1] - k[1]) * (s[1] - k[1]) + (s[2] - k[2]) * (s[2] - k[2])) / 1.732f;
            }
            return (d - tol) / fall;
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        float c[3];
        paramC(0, c);
        gpuKey(ctx, *this, in, out, R"(
    float d = YCC ? length(rgbToYCbCr(s.rgb).yz - rgbToYCbCr(k).yz) * 2.0 : length(s.rgb - k) / 1.732;
    m = (d - P[3]) / P[4];
)",
               {c[0], c[1], c[2], paramF(1), std::max(paramF(2), 1e-4f)}, false, true,
               paramI(3) == 1 ? "const bool YCC = true;\n" : "const bool YCC = false;\n");
    }
};

class ChromaKeyNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.chroma_key", "Chroma Key", "Matte",
                  {{"Image", PinType::Image}, {"Key", PinType::Image}},
                  {{"Matte", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Color("Key Color", 0.1f, 0.8f, 0.1f), ParamDesc::Float("Acceptance", 25.0f, 1.0f, 80.0f),
                   ParamDesc::Float("Falloff", 15.0f, 0.0f, 60.0f), ParamDesc::Float("Min Saturation", 0.2f, 0.0f, 1.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        KeySource key{toImage(in[1], src->w, src->h), {}};
        paramC(0, key.color);
        const float acc = paramF(1), fall = std::max(paramF(2), 1e-3f), minSat = paramF(3);
        // Compare hue angles in the CbCr plane; pixels too gray to have a reliable hue are kept.
        keyOutputs(*src, false, out, [&](const float* s, int x, int y) {
            const float* k = key.at(x, y);
            float ys, cbs, crs, yk, cbk, crk;
            rgbToYCbCr(s[0], s[1], s[2], ys, cbs, crs);
            rgbToYCbCr(k[0], k[1], k[2], yk, cbk, crk);
            float ax = cbs - 0.5f, ay = crs - 0.5f, bx = cbk - 0.5f, by = crk - 0.5f;
            float ma = std::hypot(ax, ay), mb = std::hypot(bx, by);
            if (mb < 1e-5f || ma < minSat * mb) return 1.0f;
            float ang = std::acos(std::clamp((ax * bx + ay * by) / (ma * mb), -1.0f, 1.0f)) * 180.0f / kPi;
            return (ang - acc) / fall;
        });
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        float c[3];
        paramC(0, c);
        gpuKey(ctx, *this, in, out, R"(
    vec2 a = rgbToYCbCr(s.rgb).yz - 0.5, b = rgbToYCbCr(k).yz - 0.5;
    float ma = length(a), mb = length(b);
    if (mb < 1e-5 || ma < P[5] * mb) m = 1.0;
    else m = (acos(clamp(dot(a, b) / (ma * mb), -1.0, 1.0)) * (180.0 / 3.14159265) - P[3]) / P[4];
)",
               {c[0], c[1], c[2], paramF(1), std::max(paramF(2), 1e-3f), paramF(3)}, false, true);
    }
};

class ColorSpillNode : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    NODELAB_NODE({"matte.color_spill", "Color Spill", "Matte",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f), ParamDesc::Enum("Spill Channel", 1, {"Red", "Green", "Blue"}),
                   ParamDesc::Enum("Limit", 1, {"Single (next channel)", "Average of others"}),
                   ParamDesc::Float("Ratio", 1.0f, 0.5f, 1.5f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr fac = channelOr(in[1], 1.0f);
        ChannelSampler sf = paramSampler(*this, 1, fac, src->w, src->h);
        const int c = paramI(1), o1 = (c + 1) % 3, o2 = (c + 2) % 3;
        const bool avg = paramI(2) == 1;
        const float ratio = paramF(3);
        // Spill = how far the spill channel exceeds its limit; subtract it.
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            std::copy(s, s + 4, d);
            float limit = avg ? (s[o1] + s[o2]) * 0.5f : s[o1];
            float spill = std::max(0.0f, s[c] - limit * ratio);
            d[c] = s[c] - spill * sf(x, y);
        })));
    }
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int c = paramI(1);
        gpu::PointOp g;
        g.functions = "const int C = " + std::to_string(c) + ", O1 = " + std::to_string((c + 1) % 3) +
                      ", O2 = " + std::to_string((c + 2) % 3) + ";\nconst bool AVG = " + (paramI(2) == 1 ? "true" : "false") + ";\n";
        g.body = R"(
    vec4 s = img0(p);
    float limit = AVG ? (s[O1] + s[O2]) * 0.5 : s[O1];
    float spill = max(0.0, s[C] - limit * P[0]);
    out0 = s;
    out0[C] = s[C] - spill * par1(p);
)";
        g.params = {paramF(3)};
        g.defaults = {NAN, 1.0f};
        gpu::runOver(ctx, *this, g, in, out);
    }
};

class DoubleEdgeMaskNode : public Node {
public:
    NODELAB_NODE({"matte.double_edge_mask", "Double Edge Mask", "Matte",
                  {{"Inner Mask", PinType::Channel}, {"Outer Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ChannelPtr inner = toChannel(in[0]), outer = toChannel(in[1]);
        if (!inner || !outer) return;
        int w = !outer->constant ? outer->w : (!inner->constant ? inner->w : ctx.defaultW);
        int h = !outer->constant ? outer->h : (!inner->constant ? inner->h : ctx.defaultH);
        ChannelSampler si{inner.get(), w, h}, so{outer.get(), w, h};
        std::vector<uint8_t> in_(size_t(w) * h), out_(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                in_[size_t(y) * w + x] = si(x, y) >= 0.5f;
                out_[size_t(y) * w + x] = so(x, y) < 0.5f;  // outside of the outer mask
            }
        auto dIn = imageops::distanceTransform(in_, w, h);
        auto dOut = imageops::distanceTransform(out_, w, h);
        // Gradient from 1 at the inner edge to 0 at the outer edge.
        out[0] = Value(ChannelPtr(makeChannel(w, h, [&](int x, int y) {
            size_t i = size_t(y) * w + x;
            if (in_[i]) return 1.0f;
            if (out_[i]) return 0.0f;
            return dOut[i] / std::max(dIn[i] + dOut[i], 1e-4f);
        })));
    }
};

}  // namespace

// ---------------------------------------------------------------- brush

void paintStrokes(std::vector<float>& mask, int w, int h, const std::vector<BrushMaskNode::Stroke>& strokes) {
    const float longEdge = float(std::max(w, h));
    std::vector<float> cov(mask.size(), 0.0f);  // coverage of the stroke being drawn
    for (const BrushMaskNode::Stroke& st : strokes) {
        if (st.pts.empty()) continue;
        const float r = std::max(st.radius * longEdge, 0.5f);
        const float inner = r * (1.0f - std::clamp(st.feather, 0.0f, 1.0f));
        // Points in pixels, and the stroke's bounding box.
        std::vector<std::array<float, 2>> p(st.pts.size());
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        for (size_t i = 0; i < p.size(); ++i) {
            p[i] = {st.pts[i][0] * w, st.pts[i][1] * h};
            bx0 = std::min(bx0, p[i][0]), bx1 = std::max(bx1, p[i][0]);
            by0 = std::min(by0, p[i][1]), by1 = std::max(by1, p[i][1]);
        }
        const int x0 = std::max(0, int(std::floor(bx0 - r))), x1 = std::min(w - 1, int(std::ceil(bx1 + r)));
        const int y0 = std::max(0, int(std::floor(by0 - r))), y1 = std::min(h - 1, int(std::ceil(by1 + r)));
        if (x0 > x1 || y0 > y1) continue;
        // Within one stroke coverage is the max over its segments, so a slow drag (many points
        // close together) doesn't build up more than a fast one; strokes then accumulate.
        parallelFor(y1 - y0 + 1, [&](int row) {
            const int y = y0 + row;
            const float py = y + 0.5f;
            for (size_t s = 0; s < p.size(); ++s) {
                const auto& a = p[s];
                const auto& b = s + 1 < p.size() ? p[s + 1] : p[s];
                if (py < std::min(a[1], b[1]) - r || py > std::max(a[1], b[1]) + r) continue;
                const int sx0 = std::max(x0, int(std::floor(std::min(a[0], b[0]) - r)));
                const int sx1 = std::min(x1, int(std::ceil(std::max(a[0], b[0]) + r)));
                const float ex = b[0] - a[0], ey = b[1] - a[1], el = ex * ex + ey * ey;
                for (int x = sx0; x <= sx1; ++x) {
                    const float px = x + 0.5f;
                    float t = el > 0 ? std::clamp(((px - a[0]) * ex + (py - a[1]) * ey) / el, 0.0f, 1.0f) : 0.0f;
                    const float d = std::hypot(px - a[0] - t * ex, py - a[1] - t * ey);
                    if (d >= r) continue;
                    const float c = (1.0f - smoothstep(inner, r, d)) * st.flow;
                    float& cv = cov[size_t(y) * w + x];
                    if (c > cv) cv = c;
                }
            }
        });
        parallelFor(y1 - y0 + 1, [&](int row) {
            const int y = y0 + row;
            for (int x = x0; x <= x1; ++x) {
                float& cv = cov[size_t(y) * w + x];
                if (cv <= 0) continue;
                float& m = mask[size_t(y) * w + x];
                m = st.erase ? m * (1.0f - cv) : m + (1.0f - m) * cv;
                cv = 0.0f;
            }
        });
    }
}

void BrushMaskNode::evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) {
    ChannelPtr base = toChannel(in[0]);
    int w, h;
    maskSize(base, ctx, w, h);
    auto ch = std::make_shared<Channel>(Channel::makeSized(w, h));
    if (base) {
        ChannelSampler sb{base.get(), w, h};
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) ch->data[size_t(y) * w + x] = clamp01(sb(x, y));
        });
    }
    paintStrokes(ch->data, w, h, strokes);
    if (paramB(3))
        for (float& v : ch->data) v = 1.0f - v;
    out[0] = Value(ChannelPtr(ch));
}

void BrushMaskNode::saveExtra(nlohmann::json& j) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const Stroke& s : strokes) {
        nlohmann::json pts = nlohmann::json::array();
        for (const auto& p : s.pts) pts.push_back({p[0], p[1]});
        arr.push_back({{"radius", s.radius}, {"feather", s.feather}, {"flow", s.flow}, {"erase", s.erase}, {"pts", pts}});
    }
    j["strokes"] = arr;
}

void BrushMaskNode::loadExtra(const nlohmann::json& j) {
    strokes.clear();
    auto it = j.find("strokes");
    if (it == j.end() || !it->is_array()) return;
    for (const auto& o : *it) {
        if (!o.is_object()) continue;
        Stroke s;
        s.radius = o.value("radius", 0.04f);
        s.feather = o.value("feather", 0.5f);
        s.flow = o.value("flow", 1.0f);
        s.erase = o.value("erase", false);
        if (auto p = o.find("pts"); p != o.end() && p->is_array())
            for (const auto& q : *p)
                if (q.is_array() && q.size() == 2 && q[0].is_number() && q[1].is_number())
                    s.pts.push_back({q[0].get<float>(), q[1].get<float>()});
        strokes.push_back(std::move(s));
    }
}

std::string BrushMaskNode::signatureExtra() const {
    // FNV-1a over the stroke data: cheaper than serializing thousands of points every evaluation.
    uint64_t hsh = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t n) {
        const auto* b = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; ++i) hsh = (hsh ^ b[i]) * 1099511628211ull;
    };
    for (const Stroke& s : strokes) {
        mix(&s.radius, sizeof s.radius), mix(&s.feather, sizeof s.feather), mix(&s.flow, sizeof s.flow);
        mix(&s.erase, sizeof s.erase);
        if (!s.pts.empty()) mix(s.pts.data(), s.pts.size() * sizeof s.pts[0]);
    }
    return "brush:" + std::to_string(strokes.size()) + ":" + std::to_string(hsh);
}

void BrushMaskNode::beginStroke(float u, float v, bool erase) {
    Stroke s;
    s.radius = paramF(0);
    s.feather = paramF(1);
    s.flow = paramF(2);
    s.erase = erase;
    s.pts.push_back({u, v});
    strokes.push_back(std::move(s));
}

bool BrushMaskNode::extendStroke(float u, float v, int imageW, int imageH) {
    if (strokes.empty()) return false;
    Stroke& s = strokes.back();
    const float longEdge = float(std::max(imageW, imageH));
    const auto& last = s.pts.back();
    // Points closer than a fifth of the radius add nothing visible.
    const float d = std::hypot((u - last[0]) * imageW, (v - last[1]) * imageH);
    if (d < std::max(s.radius * longEdge * 0.2f, 1.0f)) return false;
    s.pts.push_back({u, v});
    return true;
}

void registerMatteNodes(NodeRegistry& r) {
    r.add<BoxMaskNode>();
    r.add<EllipseMaskNode>();
    r.add<RadialGradientNode>();
    r.add<LinearGradientNode>();
    r.add<BrushMaskNode>();
    r.add<ChannelKeyNode>();
    r.add<LuminanceKeyNode>();
    r.add<DifferenceKeyNode>();
    r.add<DistanceKeyNode>();
    r.add<ChromaKeyNode>();
    r.add<ColorSpillNode>();
    r.add<DoubleEdgeMaskNode>();
}
