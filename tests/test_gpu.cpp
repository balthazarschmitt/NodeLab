// GPU compositing (EvalContext::gpu): every node that runs on the GPU must match its CPU version.
// Skipped on machines without a usable GPU (OpenGL 4.3).
#include <doctest/doctest.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

#include "graph/Evaluator.h"
#include "graph/NodeRegistry.h"
#include "gpu/Device.h"

namespace {

// GLFW and the device, started once for the whole run.
struct GpuSession {
    bool ok = false;
    std::string why;
    GpuSession() {
        if (!glfwInit()) {
            why = "GLFW failed to start";
            return;
        }
        ok = gpu::init(&why);
    }
    ~GpuSession() {
        gpu::shutdown();
        glfwTerminate();
    }
};

bool gpuReady() {
    static GpuSession s;
    if (!s.ok) MESSAGE("no GPU, skipping: " << s.why);
    return s.ok;
}

// A 37x23 colourful source (odd sizes catch workgroup edge bugs) with values above 1 and below 0
// so unclamped maths is exercised too. Smooth: float and double may round to either side of a jump.
Node* source(Graph& g) {
    Node* e = g.addNode("conv.image_expression");
    e->params[0] = "1.3 * u - 0.1";
    e->params[1] = "0.5 + 0.6 * sin(v * 9 + u * 4)";
    e->params[2] = "0.5 + 0.5 * cos(u * 7 + v * 3)";
    return e;
}

// The same on the CPU only (fac() has no GPU version), so a node under test gets the CPU's inputs
// and only its own maths runs in float.
Node* cpuSource(Graph& g) {
    Node* e = source(g);
    for (int k = 0; k < 3; ++k) e->params[size_t(k)] = e->params[size_t(k)].get<std::string>() + " + 0 * fac(0 * x)";
    return e;
}

struct Run {
    Value v;
    bool onGpu = false;
    int fallbacks = 0;
    std::string error;
};

Run evaluate(Graph& g, int id, int pin, bool gpu, bool linear, int w = 37, int h = 23) {
    EvalContext ctx;
    ctx.defaultW = w;
    ctx.defaultH = h;
    ctx.gpu = gpu;
    ctx.gpuHalf = false;
    g.colorManagement.linear = linear;
    Evaluator ev;
    Run r;
    r.v = toCpu(ev.evaluateOutput(g, id, pin, ctx));
    r.onGpu = ev.gpuNodes()[id];
    r.fallbacks = ev.gpuFallbacks;
    r.error = ev.lastGpuError;
    return r;
}

// Largest difference between two results, relative above 1 (half-open ranges and big values
// differ in their last float bits between the CPU and the GPU).
float difference(const Value& a, const Value& b) {
    int w = 0, h = 0;
    if (!a.size(w, h)) {
        if (b.size(w, h)) return 1e9f;
        return std::fabs(toNumber(a, 0.0f) - toNumber(b, 0.0f));
    }
    int bw, bh;
    if (!b.size(bw, bh) || bw != w || bh != h) return 1e9f;
    float worst = 0.0f;
    if (std::get_if<ImagePtr>(&a.v)) {
        ImagePtr ia = toImage(a, w, h), ib = toImage(b, w, h);
        for (size_t i = 0; i < ia->px.size(); ++i) {
            const float x = ia->px[i], y = ib->px[i];
            if (std::isnan(x) || std::isnan(y)) {
                if (std::isnan(x) != std::isnan(y)) return 1e9f;
                continue;
            }
            worst = std::max(worst, std::fabs(x - y) / std::max(1.0f, std::fabs(x)));
        }
    } else {
        ChannelPtr ca = toChannel(a), cb = toChannel(b);
        for (size_t i = 0; i < ca->data.size(); ++i)
            worst = std::max(worst, std::fabs(ca->data[i] - cb->data[i]) / std::max(1.0f, std::fabs(ca->data[i])));
    }
    return worst;
}

}  // namespace

TEST_CASE("GPU nodes match their CPU versions") {
    if (!gpuReady()) return;
    int gpuTypes = 0;
    for (const auto& type : NodeRegistry::instance().types()) {
        const NodeInfo* inf = NodeRegistry::instance().find(type);
        if (inf->hidden) continue;
        // The variants: each enum option and each bool flipped, one at a time.
        std::vector<std::pair<int, float>> variants{{-1, 0.0f}};
        for (size_t p = 0; p < inf->params.size(); ++p) {
            const ParamDesc& d = inf->params[p];
            if (d.kind == ParamKind::Enum)
                for (size_t o = 0; o < d.options.size(); ++o) variants.push_back({int(p), float(o)});
            if (d.kind == ParamKind::Bool) variants.push_back({int(p), d.def.get<bool>() ? 0.0f : 1.0f});
        }
        bool ranOnGpu = false;
        // Inputs: all connected (images to image pins, a channel to the rest), none, the first only.
        for (int wiring = 0; wiring < 3; ++wiring) {
            for (const auto& [param, value] : variants) {
                for (bool linear : {false, true}) {
                    // CPU inputs, so only the node's own maths differs (ratios such as HSV's
                    // saturation amplify the source's float rounding near black).
                    Graph g;
                    Node* src = cpuSource(g);
                    Node* split = g.addNode("color.split_rgb");
                    g.connect(src->id, 0, split->id, 0);
                    Node* n = g.addNode(type);
                    if (param >= 0) {
                        if (inf->params[size_t(param)].kind == ParamKind::Bool) n->params[size_t(param)] = value != 0.0f;
                        else n->params[size_t(param)] = int(value);
                    }
                    for (int i = 0; i < int(inf->inputs.size()); ++i) {
                        if (wiring == 1 || (wiring == 2 && i > 0)) continue;
                        if (inf->inputs[i].type == PinType::Image) g.connect(src->id, 0, n->id, i);
                        else g.connect(split->id, i % 3, n->id, i);
                    }
                    for (int o = 0; o < int(inf->outputs.size()); ++o) {
                        CAPTURE(type);
                        CAPTURE(wiring);
                        CAPTURE(param);
                        CAPTURE(value);
                        CAPTURE(linear);
                        CAPTURE(o);
                        const Run cpu = evaluate(g, n->id, o, false, linear);
                        const Run gpu = evaluate(g, n->id, o, true, linear);
                        CHECK_MESSAGE(gpu.fallbacks == 0, gpu.error);
                        if (!gpu.onGpu) continue;
                        ranOnGpu = true;
                        CHECK(cpu.v.empty() == gpu.v.empty());
                        if (!cpu.v.empty() && !gpu.v.empty()) CHECK(difference(cpu.v, gpu.v) < 2e-4f);
                    }
                }
            }
        }
        if (ranOnGpu) ++gpuTypes;
    }
    MESSAGE(gpuTypes << " node types run on the GPU");
    CHECK(gpuTypes >= 2);
}

TEST_CASE("GPU evaluation keeps values on the device between GPU nodes") {
    if (!gpuReady()) return;
    Graph g;
    Node* src = source(g);
    Node* a = g.addNode("math.mix");
    Node* b = g.addNode("math.blend");
    g.connect(src->id, 0, a->id, 0);
    g.connect(src->id, 0, a->id, 1);
    g.connect(a->id, 0, b->id, 0);
    g.connect(src->id, 0, b->id, 1);
    EvalContext ctx;
    ctx.defaultW = 64;
    ctx.defaultH = 48;
    ctx.gpu = true;
    Evaluator ev;
    Value v = ev.evaluateOutput(g, b->id, 0, ctx);
    CHECK(v.onGpu());
    CHECK(ev.gpuRuns == 3);  // the source is an Image Expression
    CHECK(ev.gpuFallbacks == 0);
    // Half precision stays within half's resolution of the full-precision CPU result.
    ctx.gpu = false;
    Evaluator cpu;
    CHECK(difference(cpu.evaluateOutput(g, b->id, 0, ctx), toCpu(v)) < 2e-3f);
}

TEST_CASE("GPU expressions match the CPU's") {
    if (!gpuReady()) return;
    // Every expression in the examples and the infrared presets, then edge cases for each helper
    // giving C's results (division by zero, NaN, pow and logs of negatives...).
    const char* const smooth[] = {
        "clamp(0.06+0.9*pow(clamp(r,0,1),0.9)+0.04*b*(1-r),0,1)",
        "clamp(0.03+0.84*pow(clamp(r,0,1),1.05),0,1)",
        "clamp(0.06+0.86*pow(clamp(r,0,1),0.95)+0.28*b*pow(1-r,2),0,1)",
        "0.2126*r + 0.7152*g + 0.0722*b",
        "abs(in1 - in2)",
        "smoothstep(0.55, 0.75, in1) * (1 - smoothstep(0.012, 0.03, in2))",
        "smoothstep(0.10, 0.22, (max(max(r,g),b) - min(min(r,g),b)) / (max(max(r,g),b) + 0.06)) * smoothstep(-10, 15, in1*360) * (1 - smoothstep(170, 195, in1*360)) * (1 - in2)",
        "smoothstep(185, 205, in1*360) * (1 - smoothstep(250, 265, in1*360)) * smoothstep(0.08, 0.2, (max(max(r,g),b) - min(min(r,g),b)) / (max(max(r,g),b) + 0.06)) * smoothstep(0.2, 0.4, in2)",
        "max(in1, in2)",
        "clamp(max(in1, in2 * 1.5), 0, 1)",
        "max(r, g * (1 - b))",
        "r*in1",
        "r/(in1+0.0001)",
        "smoothstep(0.05, 0.2, in1) * (1 - smoothstep(0.06, 0.14, max(max(r,g),b)))",
        "r * (1 - in1)",
        "max(r, g) * (1 - b)",
        "mix(mix(0.05 + 0.85*pow(max(r,0),0.75), 0.1 + 0.9*sqrt(max(r,0)), g), -0.12 + 0.62*r, b)",
        "mix(r, g, 0.5*b)",
        "in1 + 0.25*max(in2 - 0.6, 0)",
        "1/(r-r) + (r-r)/(r-r) - 1/(r-r)",
        "(1/(r-r) > 5) + (sqrt(r-2) == sqrt(r-2)) * 10",
        "pow(-r, 2) + pow(-r, 3) + pow(-r, 0.5) + pow(r - r, -1) + pow(-2, 3)",
        "(pow(-r, 0.5) != pow(-r, 0.5)) + 2 * (pow(r, 0) == 1)",
        "ln(r) + log(g) + log10(g + 1)",
        "ln(r - r) < -1000",
        "atan2(r - 0.5, g - 0.5) + atan2(r - r, g - g) + atan(b)",
        "acos(r) + asin(g * 2 - 1) + 3 * (acos(r + 5) != acos(r + 5))",
        "tanh(x - 18) + tanh(x * 50) + sinh(g) + cosh(g) + exp(x / 8) / 100 + tan(u)",
        "r^2 + -r^0.5 + 2^u",
        "clamp(r, 0.2, 0.6) + min(g, b) - max(a, 0.5) + mix(r, g, b)",
        "smoothstep(0.6, 0.2, u) + smoothstep(0.2, 0.6, v)",
        "w * h / 1000 + x * y / 500 + u + v",
        "sin(x) * cos(y) + sin(u * 20)",
        "-!(r - r) + -!!(g) + !b + !!(r - r)",
        "(r, g) + (b, 0.5)",
        "fac(3) + pi + e",
    };
    // Jumps (fract, step, comparisons, modulo) may land either side in float and double.
    const char* const jumpy[] = {
        "x % 7 + (y - 9) % 4 + r % 0.3 + (r - r) % 0",
        "step(0.5, u) + fract(v * 3.3) + floor(x / 3) + ceil(y / 4)",
        "(u < 0.5) && (v > 0.3) || !(r > 0.4)",
        "(u <= 0.5) + (v >= 0.5) * 2 + (r == g) * 4 + (r != b) * 8",
    };
    auto check = [&](const char* expr, int allowed) {
        const std::string exprText = expr;
        CAPTURE(exprText);
        Graph g;
        Node* src = cpuSource(g);
        Node* split = g.addNode("color.split_rgb");
        g.connect(src->id, 0, split->id, 0);
        Node* n = g.addNode("conv.expression");
        n->params[0] = expr;
        g.connect(src->id, 0, n->id, 0);
        g.connect(split->id, 0, n->id, 1);
        g.connect(split->id, 2, n->id, 2);
        const Run cpu = evaluate(g, n->id, 0, false, true), gpu = evaluate(g, n->id, 0, true, true);
        CHECK(gpu.onGpu);
        CHECK(gpu.fallbacks == 0);
        ChannelPtr a = toChannel(cpu.v), b = toChannel(gpu.v);
        REQUIRE(a);
        REQUIRE(b);
        REQUIRE(a->data.size() == b->data.size());
        int bad = 0;
        float worst = 0.0f;
        for (size_t i = 0; i < a->data.size(); ++i) {
            const float d = std::fabs(a->data[i] - b->data[i]) / std::max(1.0f, std::fabs(a->data[i]));
            if (d > 2e-4f) ++bad, worst = std::max(worst, d);
        }
        CAPTURE(worst);
        CHECK(bad <= allowed);
    };
    for (const char* e : smooth) check(e, 0);
    for (const char* e : jumpy) check(e, int(37 * 23 / 100));

    // Functions without a GLSL version run on the CPU.
    Graph g;
    Node* n = g.addNode("conv.expression");
    n->params[0] = "fac(x)";
    const Run r = evaluate(g, n->id, 0, true, true);
    CHECK_FALSE(r.onGpu);
    CHECK(r.fallbacks == 0);
}

TEST_CASE("GPU blur matches the CPU on large radii") {
    if (!gpuReady()) return;
    // Lines longer than a thread's 64-pixel chunk, and radii past the image edge.
    for (float size : {3.0f, 40.0f, 400.0f}) {
        for (bool channel : {false, true}) {
            CAPTURE(size);
            CAPTURE(channel);
            Graph g;
            Node* src = cpuSource(g);
            Node* blur = g.addNode("filter.blur");
            blur->params[0] = size;
            blur->params[1] = size * 0.5f;
            if (channel) {
                Node* split = g.addNode("color.split_rgb");
                g.connect(src->id, 0, split->id, 0);
                g.connect(split->id, 1, blur->id, 0);
            } else {
                g.connect(src->id, 0, blur->id, 0);
            }
            const Run cpu = evaluate(g, blur->id, 0, false, true, 211, 147);
            const Run gpu = evaluate(g, blur->id, 0, true, true, 211, 147);
            REQUIRE(gpu.onGpu);
            CHECK(difference(cpu.v, gpu.v) < 2e-4f);
        }
    }
}
