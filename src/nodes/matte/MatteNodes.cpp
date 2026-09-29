// Matte nodes: shape masks and keyers. Keyers output a Matte channel (1 = keep) and the keyed
// image (color * matte, alpha = matte).
#include <cmath>

#include "core/ColorMath.h"
#include "nodes/ImageOps.h"
#include "nodes/NodeUtil.h"

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

class ShapeMaskNode : public Node {
protected:
    // pinned param indices: 0 X, 1 Y, 2 Width, 3 Height, 4 Rotation, 5 Feather, 6 Value, 7 Operation
    void run(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out, bool ellipse) {
        ChannelPtr base = toChannel(in[0]);
        int w = base && !base->constant ? base->w : ctx.defaultW;
        int h = base && !base->constant ? base->h : ctx.defaultH;
        if (!base) base = std::make_shared<Channel>(Channel::makeConstant(0.0f));
        ChannelPtr val = channelOr(in[1], 1.0f);
        ChannelSampler sb{base.get(), w, h}, sv = paramSampler(*this, 1, val, w, h);
        const float cx = paramF(0) * w, cy = paramF(1) * h;
        const float hw = std::max(paramF(2) * w * 0.5f, 1e-3f), hh = std::max(paramF(3) * h * 0.5f, 1e-3f);
        const float a = -paramF(4) * kPi / 180.0f, ca = std::cos(a), sa = std::sin(a);
        const float feather = paramF(5);
        const int op = paramI(7);
        out[0] = Value(ChannelPtr(makeChannel(w, h, [&](int x, int y) {
            float px = x + 0.5f - cx, py = y + 0.5f - cy;
            float rx = px * ca - py * sa, ry = px * sa + py * ca;
            // Normalized "radius": <1 inside. Box uses the max norm, ellipse the Euclidean one.
            float q = ellipse ? std::hypot(rx / hw, ry / hh) : std::max(std::fabs(rx) / hw, std::fabs(ry) / hh);
            float shape = 1.0f - smoothstep(1.0f - feather, 1.0f + 1e-4f, q);
            float v = sv(x, y) * shape, b = sb(x, y);
            switch (op) {
                case 1: return clamp01(b - v);            // Subtract
                case 2: return clamp01(b * v);            // Multiply
                case 3: return clamp01(std::max(b, 1.0f - v));  // Not: everything outside the shape
                default: return clamp01(std::max(b, v));  // Add
            }
        })));
    }
};

#define SHAPE_PARAMS                                                                                        \
    {ParamDesc::Float("X", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Y", 0.5f, 0.0f, 1.0f),                    \
     ParamDesc::Float("Width", 0.4f, 0.0f, 2.0f), ParamDesc::Float("Height", 0.3f, 0.0f, 2.0f),           \
     ParamDesc::Float("Rotation", 0.0f, -180.0f, 180.0f), ParamDesc::Float("Feather", 0.1f, 0.0f, 1.0f),  \
     ParamDesc::Float("Value", 1.0f, 0.0f, 1.0f), ParamDesc::Enum("Operation", 0, {"Add", "Subtract", "Multiply", "Not"})}

class BoxMaskNode : public ShapeMaskNode {
public:
    NODELAB_NODE({"matte.box_mask", "Box Mask", "Matte",
                  {{"Mask", PinType::Channel}, {"Value", PinType::Channel, 6}},
                  {{"Mask", PinType::Channel}},
                  SHAPE_PARAMS})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { run(ctx, in, out, false); }
};

class EllipseMaskNode : public ShapeMaskNode {
public:
    NODELAB_NODE({"matte.ellipse_mask", "Ellipse Mask", "Matte",
                  {{"Mask", PinType::Channel}, {"Value", PinType::Channel, 6}},
                  {{"Mask", PinType::Channel}},
                  SHAPE_PARAMS})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override { run(ctx, in, out, true); }
};

// ---------------------------------------------------------------- keyers

class ChannelKeyNode : public Node {
public:
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
};

class LuminanceKeyNode : public Node {
public:
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
};

class DifferenceKeyNode : public Node {
public:
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
};

class DistanceKeyNode : public Node {
public:
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
};

class ChromaKeyNode : public Node {
public:
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
};

class ColorSpillNode : public Node {
public:
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

void registerMatteNodes(NodeRegistry& r) {
    r.add<BoxMaskNode>();
    r.add<EllipseMaskNode>();
    r.add<ChannelKeyNode>();
    r.add<LuminanceKeyNode>();
    r.add<DifferenceKeyNode>();
    r.add<DistanceKeyNode>();
    r.add<ChromaKeyNode>();
    r.add<ColorSpillNode>();
    r.add<DoubleEdgeMaskNode>();
}
