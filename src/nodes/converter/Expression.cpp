// Expression nodes: per-pixel math typed as text, compiled with tinyexpr.
//
// Variables: r g b a (the Image input), in1 in2 (channel inputs), x y (pixel), u v (0..1 across
// the image), w h (size), plus tinyexpr's pi, e. Functions: tinyexpr built-ins (sin, cos, pow,
// sqrt, abs, floor, ceil, log, ln, exp, atan2, ...) and min, max, clamp, mix, step, smoothstep, fract.
#include <cmath>
#include <stdexcept>

extern "C" {
#include <tinyexpr.h>
}

#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

double fnMin(double a, double b) { return a < b ? a : b; }
double fnMax(double a, double b) { return a > b ? a : b; }
double fnClamp(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }
double fnMix(double a, double b, double t) { return a + (b - a) * t; }
double fnStep(double edge, double x) { return x < edge ? 0.0 : 1.0; }
double fnSmoothstep(double e0, double e1, double x) {
    if (e1 <= e0) return x < e0 ? 0.0 : 1.0;
    double t = fnClamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
double fnFract(double x) { return x - std::floor(x); }

struct Vars {
    double r = 0, g = 0, b = 0, a = 1, in1 = 0, in2 = 0, x = 0, y = 0, u = 0, v = 0, w = 1, h = 1;
};

// One compiled expression bound to a Vars instance (tinyexpr reads variables through pointers).
class Program {
public:
    Program(const std::string& src, Vars& vars) {
        const te_variable table[] = {
            {"r", &vars.r, TE_VARIABLE, nullptr},     {"g", &vars.g, TE_VARIABLE, nullptr},
            {"b", &vars.b, TE_VARIABLE, nullptr},     {"a", &vars.a, TE_VARIABLE, nullptr},
            {"in1", &vars.in1, TE_VARIABLE, nullptr}, {"in2", &vars.in2, TE_VARIABLE, nullptr},
            {"x", &vars.x, TE_VARIABLE, nullptr},     {"y", &vars.y, TE_VARIABLE, nullptr},
            {"u", &vars.u, TE_VARIABLE, nullptr},     {"v", &vars.v, TE_VARIABLE, nullptr},
            {"w", &vars.w, TE_VARIABLE, nullptr},     {"h", &vars.h, TE_VARIABLE, nullptr},
            {"min", (const void*)fnMin, TE_FUNCTION2 | TE_FLAG_PURE, nullptr},
            {"max", (const void*)fnMax, TE_FUNCTION2 | TE_FLAG_PURE, nullptr},
            {"clamp", (const void*)fnClamp, TE_FUNCTION3 | TE_FLAG_PURE, nullptr},
            {"mix", (const void*)fnMix, TE_FUNCTION3 | TE_FLAG_PURE, nullptr},
            {"step", (const void*)fnStep, TE_FUNCTION2 | TE_FLAG_PURE, nullptr},
            {"smoothstep", (const void*)fnSmoothstep, TE_FUNCTION3 | TE_FLAG_PURE, nullptr},
            {"fract", (const void*)fnFract, TE_FUNCTION1 | TE_FLAG_PURE, nullptr},
        };
        expr_ = te_compile(src.c_str(), table, int(sizeof(table) / sizeof(table[0])), &errPos_);
    }
    ~Program() { te_free(expr_); }
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    bool ok() const { return expr_ != nullptr; }
    int errorPos() const { return errPos_; }
    float eval() const {
        double r = te_eval(expr_);
        return std::isfinite(r) ? float(r) : 0.0f;
    }

private:
    te_expr* expr_ = nullptr;
    int errPos_ = 0;
};

void validate(const std::string& label, const std::string& src) {
    Vars v;
    Program p(src, v);
    if (!p.ok())
        throw std::runtime_error(label + ": syntax error at character " + std::to_string(p.errorPos()) + " in \"" + src + "\"");
}

// Shared driver: fills vars per pixel, then calls emit(programs, pixelIndex).
template <typename Emit>
void runExpressions(const Node& node, EvalContext& ctx, const std::vector<Value>& in, int in1Pin, int in2Pin,
                    const std::vector<std::string>& sources, int w, int h, Emit&& emit) {
    ImagePtr img = toImage(in[0], w, h);
    ChannelPtr c1 = channelOr(in[in1Pin], 0.0f), c2 = channelOr(in[in2Pin], 0.0f);
    ChannelSampler s1 = paramSampler(node, in1Pin, c1, w, h), s2 = paramSampler(node, in2Pin, c2, w, h);
    ImageSampler si{img.get(), w, h};
    parallelForChunks(h, [&](int y0, int y1) {
        Vars vars;
        vars.w = w;
        vars.h = h;
        std::vector<std::unique_ptr<Program>> progs;
        for (const auto& s : sources) progs.push_back(std::make_unique<Program>(s, vars));
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                if (img) {
                    const float* p = si(x, y);
                    vars.r = p[0], vars.g = p[1], vars.b = p[2], vars.a = p[3];
                }
                vars.in1 = s1(x, y);
                vars.in2 = s2(x, y);
                vars.x = x;
                vars.y = y;
                vars.u = (x + 0.5) / w;
                vars.v = (y + 0.5) / h;
                emit(progs, size_t(y) * w + x);
            }
    });
    (void)ctx;
}

class ExpressionNode : public Node {
public:
    NODELAB_NODE({"conv.expression", "Expression", "Converter",
                  {{"Image", PinType::Image}, {"In1", PinType::Channel, 1}, {"In2", PinType::Channel, 2}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::Text("Expression", "(r + g + b) / 3"), ParamDesc::FloatFree("In1", 0.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("In2", 0.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const std::string src = paramS(0);
        validate("Expression", src);
        int w, h;
        resolveSize(in, ctx, w, h);
        auto ch = std::make_shared<Channel>(Channel::makeSized(w, h));
        runExpressions(*this, ctx, in, 1, 2, {src}, w, h, [&](auto& progs, size_t i) { ch->data[i] = progs[0]->eval(); });
        out[0] = Value(ChannelPtr(ch));
    }
};

class ImageExpressionNode : public Node {
public:
    NODELAB_NODE({"conv.image_expression", "Image Expression", "Converter",
                  {{"Image", PinType::Image}, {"In1", PinType::Channel, 3}, {"In2", PinType::Channel, 4}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Text("R", "r"), ParamDesc::Text("G", "g"), ParamDesc::Text("B", "b"),
                   ParamDesc::FloatFree("In1", 0.0f, 0.0f, 1.0f), ParamDesc::FloatFree("In2", 0.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const std::vector<std::string> srcs = {paramS(0), paramS(1), paramS(2)};
        validate("R", srcs[0]);
        validate("G", srcs[1]);
        validate("B", srcs[2]);
        int w, h;
        resolveSize(in, ctx, w, h);
        auto img = std::make_shared<Image>(w, h);
        ImagePtr src = toImage(in[0], w, h);
        runExpressions(*this, ctx, in, 1, 2, srcs, w, h, [&](auto& progs, size_t i) {
            float* d = img->pixel(i);
            for (int k = 0; k < 3; ++k) d[k] = progs[k]->eval();
            d[3] = (src && src->w == w && src->h == h) ? src->pixel(i)[3] : 1.0f;
        });
        out[0] = Value(ImagePtr(img));
    }
};

}  // namespace

void registerExpressionNodes(NodeRegistry& r) {
    r.add<ExpressionNode>();
    r.add<ImageExpressionNode>();
}
