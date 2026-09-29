#include <cmath>

#include "core/ColorMath.h"
#include "core/Ramp.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

// Per-pixel function of N channel inputs. If none of them has a resolution the result is a
// constant (sizeless) channel, so scalar math stays cheap.
template <int N, typename Fn>
Value channelOp(const Node& node, const std::vector<Value>& in, const int (&pins)[N], const float (&defs)[N], Fn&& fn) {
    ChannelPtr c[N];
    int w = 0, h = 0;
    bool sized = false;
    for (int k = 0; k < N; ++k) {
        c[k] = channelOr(in[pins[k]], defs[k]);
        if (!c[k]->constant && !sized) {
            w = c[k]->w;
            h = c[k]->h;
            sized = true;
        }
    }
    ChannelSampler s[N];
    for (int k = 0; k < N; ++k) s[k] = paramSampler(node, pins[k], c[k], sized ? w : 1, sized ? h : 1);
    if (!sized) {
        float v[N];
        for (int k = 0; k < N; ++k) v[k] = s[k](0, 0);
        return Value(ChannelPtr(std::make_shared<Channel>(Channel::makeConstant(fn(v)))));
    }
    return Value(ChannelPtr(makeChannel(w, h, [&](int x, int y) {
        float v[N];
        for (int k = 0; k < N; ++k) v[k] = s[k](x, y);
        return fn(v);
    })));
}

float smoothstep(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0f : 1.0f;
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// 1 inside [lo, hi], fading to 0 over `soft` outside it.
float band(float v, float lo, float hi, float soft) {
    return smoothstep(lo - soft, lo, v) * (1.0f - smoothstep(hi, hi + soft, v));
}

// ---------------------------------------------------------------- ramps / keys

class ColorRampNode : public Node {
public:
    NODELAB_NODE({"conv.color_ramp", "Color Ramp", "Converter",
                  {{"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}, {"Alpha", PinType::Channel}},
                  {ParamDesc::Float("Factor", 0.5f, 0.0f, 1.0f), ParamDesc::Ramp("Ramp")}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ColorRamp ramp = rampFromJson(params[1]);
        ChannelPtr fac = channelOr(in[0], 0.5f);
        int w = fac->constant ? ctx.defaultW : fac->w, h = fac->constant ? ctx.defaultH : fac->h;
        ChannelSampler sf = paramSampler(*this, 0, fac, w, h);
        auto img = std::make_shared<Image>(w, h);
        auto alpha = std::make_shared<Channel>(Channel::makeSized(w, h));
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                size_t i = size_t(y) * w + x;
                float c[4];
                ramp.eval(sf(x, y), c);
                float* d = img->pixel(i);
                d[0] = c[0], d[1] = c[1], d[2] = c[2], d[3] = 1.0f;
                alpha->data[i] = c[3];
            }
        });
        out[0] = Value(ImagePtr(img));
        out[1] = Value(ChannelPtr(alpha));
    }
};

class ColorKeyNode : public Node {
public:
    NODELAB_NODE({"conv.color_key", "Color Key", "Converter",
                  {{"Image", PinType::Image}},
                  {{"Mask", PinType::Channel}, {"Image", PinType::Image}},
                  {ParamDesc::Float("Hue", 120.0f, 0.0f, 360.0f), ParamDesc::Float("Hue Range", 30.0f, 0.0f, 180.0f),
                   ParamDesc::Float("Sat Min", 0.15f, 0.0f, 1.0f), ParamDesc::Float("Sat Max", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Value Min", 0.05f, 0.0f, 1.0f), ParamDesc::Float("Value Max", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Softness", 0.1f, 0.0f, 0.5f), ParamDesc::Bool("Invert", false)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float hue = paramF(0) / 360.0f, range = paramF(1) / 360.0f;
        const float smin = paramF(2), smax = paramF(3), vmin = paramF(4), vmax = paramF(5), soft = paramF(6);
        const bool inv = paramB(7);
        auto mask = std::make_shared<Channel>(Channel::makeSized(src->w, src->h));
        auto keyed = mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float h, sat, v;
            colormath::rgbToHsv(clamp01(s[0]), clamp01(s[1]), clamp01(s[2]), h, sat, v);
            float dh = std::fabs(h - hue);
            dh = std::min(dh, 1.0f - dh);  // hue is circular
            float m = (1.0f - smoothstep(range, range + soft * 0.5f, dh)) * band(sat, smin, smax, soft) *
                      band(v, vmin, vmax, soft);
            if (range >= 0.5f) m = band(sat, smin, smax, soft) * band(v, vmin, vmax, soft);
            if (inv) m = 1.0f - m;
            mask->data[size_t(y) * src->w + x] = m;
            for (int k = 0; k < 3; ++k) d[k] = s[k] * m;  // keyed color, black elsewhere
            d[3] = s[3];
        });
        out[0] = Value(ChannelPtr(mask));
        out[1] = Value(ImagePtr(keyed));
    }
};

// ---------------------------------------------------------------- scalar math

class MapRangeNode : public Node {
public:
    NODELAB_NODE({"conv.map_range", "Map Range", "Converter",
                  {{"Value", PinType::Channel, 0}, {"From Min", PinType::Channel, 1}, {"From Max", PinType::Channel, 2},
                   {"To Min", PinType::Channel, 3}, {"To Max", PinType::Channel, 4}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::FloatFree("Value", 0.5f, 0.0f, 1.0f), ParamDesc::FloatFree("From Min", 0.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("From Max", 1.0f, 0.0f, 1.0f), ParamDesc::FloatFree("To Min", 0.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("To Max", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Enum("Interpolation", 0, {"Linear", "Smooth Step", "Stepped (4)", "Stepped (8)"}),
                   ParamDesc::Bool("Clamp", true)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int interp = paramI(5);
        const bool clampOut = paramB(6);
        const int pins[5] = {0, 1, 2, 3, 4};
        const float defs[5] = {0.5f, 0, 1, 0, 1};
        out[0] = channelOp<5>(*this, in, pins, defs, [&](const float* v) {
            float t = (v[0] - v[1]) / (std::fabs(v[2] - v[1]) < 1e-9f ? 1e-9f : (v[2] - v[1]));
            if (clampOut) t = std::clamp(t, 0.0f, 1.0f);
            if (interp == 1) t = smoothstep(0.0f, 1.0f, t);
            else if (interp == 2) t = std::floor(t * 4.0f) / 4.0f;
            else if (interp == 3) t = std::floor(t * 8.0f) / 8.0f;
            return v[3] + t * (v[4] - v[3]);
        });
    }
};

class ClampNode : public Node {
public:
    NODELAB_NODE({"conv.clamp", "Clamp", "Converter",
                  {{"Value", PinType::Channel, 0}, {"Min", PinType::Channel, 1}, {"Max", PinType::Channel, 2}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::FloatFree("Value", 0.5f, 0.0f, 1.0f), ParamDesc::FloatFree("Min", 0.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("Max", 1.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int pins[3] = {0, 1, 2};
        const float defs[3] = {0.5f, 0, 1};
        out[0] = channelOp<3>(*this, in, pins, defs, [](const float* v) {
            return std::clamp(v[0], std::min(v[1], v[2]), std::max(v[1], v[2]));
        });
    }
};

class ThresholdNode : public Node {
public:
    NODELAB_NODE({"conv.threshold", "Threshold", "Converter",
                  {{"Value", PinType::Channel, 0}, {"Threshold", PinType::Channel, 1}, {"Softness", PinType::Channel, 2}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Float("Value", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Threshold", 0.5f, 0.0f, 1.0f),
                   ParamDesc::Float("Softness", 0.0f, 0.0f, 0.5f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int pins[3] = {0, 1, 2};
        const float defs[3] = {0.5f, 0.5f, 0};
        out[0] = channelOp<3>(*this, in, pins, defs, [](const float* v) {
            return smoothstep(v[1] - v[2], v[1] + v[2], v[0]);
        });
    }
};

class MathNode : public Node {
public:
    enum Op {
        Add, Subtract, Multiply, Divide, Power, Logarithm, SquareRoot, Absolute, Minimum, Maximum, LessThan,
        GreaterThan, Modulo, Floor, Ceil, Round, Fract, Sine, Cosine, Snap, PingPong
    };
    NODELAB_NODE({"conv.math", "Math", "Converter",
                  {{"A", PinType::Channel, 0}, {"B", PinType::Channel, 1}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::FloatFree("A", 0.5f, 0.0f, 1.0f), ParamDesc::FloatFree("B", 0.5f, 0.0f, 1.0f),
                   ParamDesc::Enum("Operation", Add,
                                   {"Add", "Subtract", "Multiply", "Divide", "Power", "Logarithm (base B)", "Square Root",
                                    "Absolute", "Minimum", "Maximum", "Less Than", "Greater Than", "Modulo", "Floor",
                                    "Ceil", "Round", "Fraction", "Sine", "Cosine", "Snap (to B)", "Ping-Pong (B)"}),
                   ParamDesc::Bool("Clamp", false)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int op = paramI(2);
        const bool clampOut = paramB(3);
        const int pins[2] = {0, 1};
        const float defs[2] = {0.5f, 0.5f};
        out[0] = channelOp<2>(*this, in, pins, defs, [&](const float* v) {
            float a = v[0], b = v[1], r = 0.0f;
            switch (op) {
                case Add: r = a + b; break;
                case Subtract: r = a - b; break;
                case Multiply: r = a * b; break;
                case Divide: r = std::fabs(b) < 1e-9f ? 0.0f : a / b; break;
                case Power: r = (a < 0 && b != std::floor(b)) ? 0.0f : std::pow(a, b); break;
                case Logarithm: r = (a > 0 && b > 0 && b != 1) ? std::log(a) / std::log(b) : 0.0f; break;
                case SquareRoot: r = a > 0 ? std::sqrt(a) : 0.0f; break;
                case Absolute: r = std::fabs(a); break;
                case Minimum: r = std::min(a, b); break;
                case Maximum: r = std::max(a, b); break;
                case LessThan: r = a < b ? 1.0f : 0.0f; break;
                case GreaterThan: r = a > b ? 1.0f : 0.0f; break;
                case Modulo: r = std::fabs(b) < 1e-9f ? 0.0f : a - b * std::floor(a / b); break;
                case Floor: r = std::floor(a); break;
                case Ceil: r = std::ceil(a); break;
                case Round: r = std::round(a); break;
                case Fract: r = a - std::floor(a); break;
                case Sine: r = std::sin(a); break;
                case Cosine: r = std::cos(a); break;
                case Snap: r = std::fabs(b) < 1e-9f ? a : std::floor(a / b) * b; break;
                case PingPong: {
                    if (std::fabs(b) < 1e-9f) break;
                    float t = a / b - std::floor(a / b * 0.5f) * 2.0f;
                    r = (t > 1.0f ? 2.0f - t : t) * b;
                    break;
                }
            }
            return clampOut ? std::clamp(r, 0.0f, 1.0f) : r;
        });
    }
};

}  // namespace

void registerExpressionNodes(NodeRegistry& r);  // Expression.cpp

void registerConverterNodes(NodeRegistry& r) {
    r.add<ColorRampNode>();
    r.add<ColorKeyNode>();
    r.add<MapRangeNode>();
    r.add<MathNode>();
    r.add<ClampNode>();
    r.add<ThresholdNode>();
    registerExpressionNodes(r);
}
