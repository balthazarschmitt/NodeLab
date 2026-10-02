// GPU compositing (EvalContext::gpu): every node that runs on the GPU must match its CPU version.
// Skipped on machines without a usable GPU (OpenGL 4.3).
#include <doctest/doctest.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

#include <filesystem>
#include "graph/Evaluator.h"
#include "io/Export.h"
#include "io/ImageIO.h"
#include "io/Paths.h"
#include "graph/NodeRegistry.h"
#include "core/ColorManagement.h"
#include "gpu/Device.h"
#include "gpu/Blur.h"
#include "gpu/Display.h"
#include "nodes/ImageOps.h"
#include "gpu/PointOp.h"

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
    ctx.colorManagement = g.colorManagement;  // as initContextSize does
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

bool gpuTestDevice() { return gpuReady(); }

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

TEST_CASE("GPU curve nodes match the CPU with bent curves") {
    if (!gpuReady()) return;
    // The generic test only sees flat curves; these exercise the lookup tables on the device.
    const nlohmann::json bent = {{0.0f, 0.1f}, {0.3f, 0.6f}, {0.7f, 0.5f}, {1.0f, 0.95f}};
    const nlohmann::json hue = {{0.0f, 0.3f}, {0.4f, 0.8f}, {0.8f, 0.45f}, {1.0f, 0.3f}};
    struct Case {
        const char* type;
        nlohmann::json curves;
    } cases[] = {
        {"color.curves", {{"master", bent}, {"r", hue}, {"g", bent}}},
        {"color.hue_correct", {{"h", hue}, {"s", bent}, {"v", hue}}},
    };
    for (const Case& c : cases) {
        for (bool linear : {false, true}) {
            const std::string type = c.type;
            CAPTURE(type);
            CAPTURE(linear);
            Graph g;
            Node* src = cpuSource(g);
            Node* n = g.addNode(c.type);
            n->params[0] = 0.8f;
            n->params[1] = c.curves;
            g.connect(src->id, 0, n->id, 0);
            const Run cpu = evaluate(g, n->id, 0, false, linear), gpu = evaluate(g, n->id, 0, true, linear);
            CHECK(gpu.onGpu);
            CHECK(gpu.fallbacks == 0);
            CHECK(difference(cpu.v, gpu.v) < 2e-4f);
        }
    }
}

TEST_CASE("GPU develop nodes match the CPU with sliders moved") {
    if (!gpuReady()) return;
    struct Case {
        const char* type;
        std::vector<std::pair<int, float>> params;
        bool linear;
    };
    const std::vector<std::pair<int, float>> basic = {{1, 30}, {2, -20}, {3, 0.7f}, {4, 40}, {7, 30}, {8, -20}, {12, 40}, {13, 20}};
    std::vector<std::pair<int, float>> basicLegacy = basic;
    basicLegacy.insert(basicLegacy.end(), {{5, -50}, {6, 40}});
    const std::vector<std::pair<int, float>> mixer = {{1, 60}, {3, -40}, {6, 80}, {9, -50}, {11, 70}, {14, 30}, {17, -60}, {20, 45}, {22, 25}};
    const std::vector<std::pair<int, float>> grading = {{1, 200}, {2, 60}, {3, -30}, {5, 40}, {6, 20}, {7, 30}, {8, 70},
                                                        {9, 25}, {11, 15}, {12, -10}, {13, 30}, {14, -40}};
    const Case cases[] = {
        {"color.basic", basic, true},    {"color.basic", basicLegacy, false}, {"color.color_mixer", mixer, true},
        {"color.color_mixer", mixer, false}, {"color.color_grading", grading, true}, {"color.color_grading", grading, false},
    };
    for (const Case& c : cases) {
        for (float factor : {1.0f, 0.6f}) {
            const std::string type = c.type;
            CAPTURE(type);
            CAPTURE(c.linear);
            CAPTURE(factor);
            Graph g;
            Node* src = cpuSource(g);
            Node* n = g.addNode(c.type);
            n->params[0] = factor;
            for (const auto& [i, v] : c.params) n->params[size_t(i)] = v;
            g.connect(src->id, 0, n->id, 0);
            const Run cpu = evaluate(g, n->id, 0, false, c.linear), gpu = evaluate(g, n->id, 0, true, c.linear);
            CHECK(gpu.onGpu);
            CHECK(gpu.fallbacks == 0);
            CHECK(difference(cpu.v, gpu.v) < 2e-4f);
        }
    }
}

TEST_CASE("GPU Basic matches the CPU with its local filters") {
    if (!gpuReady()) return;
    // Highlights/Shadows (the tone equalizer's guided mask in linear projects), Clarity (guided),
    // Texture (blur) and Dehaze (dark channel, airlight). The large size makes the fast guided
    // filter solve its coefficients on cells.
    const std::vector<std::vector<std::pair<int, float>>> cases = {
        {{5, -50}, {6, 40}},
        {{10, 60}},
        {{10, -40}, {3, 0.5f}},
        {{9, 50}},
        {{11, 40}},
        {{11, -40}},
        {{1, 20}, {4, 30}, {5, -60}, {6, 50}, {9, -30}, {10, 40}, {11, 30}, {12, 30}},
    };
    for (const auto& params : cases)
        for (bool linear : {false, true})
            for (int sz : {1, 13}) {
                CAPTURE(linear);
                CAPTURE(sz);
                CAPTURE(params.front().first);
                Graph g;
                Node* src = cpuSource(g);
                Node* n = g.addNode("color.basic");
                for (const auto& [i, v] : params) n->params[size_t(i)] = v;
                g.connect(src->id, 0, n->id, 0);
                const int w = 37 * sz, h = 23 * sz;
                const Run cpu = evaluate(g, n->id, 0, false, linear, w, h), gpu = evaluate(g, n->id, 0, true, linear, w, h);
                CHECK(gpu.onGpu);
                CHECK(gpu.fallbacks == 0);
                CHECK(difference(cpu.v, gpu.v) < 2e-4f);
            }
}

TEST_CASE("GPU filters and transforms match the CPU with params moved") {
    if (!gpuReady()) return;
    struct Case {
        const char* type;
        std::vector<std::pair<int, float>> params;
        // Extra inputs: pin and the source's channel feeding it (-1: the source image).
        std::vector<std::pair<int, int>> wires;
    };
    const Case cases[] = {
        {"xform.transform", {{0, 7}, {1, -4}, {2, 25}, {3, 1.3f}}, {}},
        {"xform.transform", {{0, 7}, {1, -4}, {2, -40}, {3, 0.7f}, {4, 1}}, {}},
        {"xform.crop", {{0, 0.1f}, {1, 0.85f}, {2, 0.2f}, {3, 0.9f}, {5, 8}}, {}},
        {"xform.crop", {{0, 0.1f}, {1, 0.85f}, {2, 0.2f}, {3, 0.9f}, {4, 0}, {5, -6}, {7, 0}}, {}},
        {"xform.crop", {{0, 0.05f}, {1, 0.9f}, {6, 2}}, {}},
        {"xform.lens_distortion", {{0, -0.3f}, {1, 0.1f}}, {}},
        {"xform.lens_distortion", {{0, 0.4f}, {1, 0.05f}, {2, 0}}, {}},
        {"xform.lens_correction", {{0, 40}, {2, 60}, {3, -50}, {4, -60}, {5, 30}}, {}},
        {"xform.lens_correction", {{0, -50}, {1, 0}, {4, 70}}, {}},
        {"xform.corner_pin", {{0, 0.1f}, {1, 0.05f}, {2, 0.95f}, {5, 0.8f}, {6, 0.2f}}, {}},
        {"xform.displace", {{0, 15}, {1, -10}}, {{1, 0}, {2, 2}}},
        {"xform.map_uv", {}, {{1, -1}}},
        {"filter.directional_blur", {{0, 8}, {1, 30}, {2, 10}, {3, 0.2f}}, {}},
        {"filter.denoise", {{0, 60}, {1, 20}, {2, 50}}, {}},
        {"filter.denoise", {{2, 100}, {3, 0}}, {}},
        {"matte.range_mask", {{1, 0.3f}, {2, 0.7f}, {3, 0.2f}}, {{1, 0}}},
        {"matte.range_mask", {{0, 1}, {5, 0.8f}, {6, 1}}, {}},
        {"filter.bilateral_blur", {{0, 5}, {1, 0.3f}}, {}},
        {"filter.bilateral_blur", {{0, 3}, {1, 0.05f}}, {{1, -1}}},
        {"filter.filter", {{1, 2}}, {{1, 0}}},
        {"filter.dilate_erode", {{1, 3.5f}}, {}},
        {"filter.dilate_erode", {{1, -4.5f}}, {}},
        {"filter.dilate_erode", {{0, 1}, {1, 6.5f}}, {}},
        {"filter.dilate_erode", {{0, 1}, {1, -2.5f}}, {}},
        {"filter.kuwahara", {{0, 3}}, {}},
        {"filter.kuwahara", {{0, 9}}, {}},
        {"filter.pixelate", {{0, 5}}, {}},
        {"filter.posterize", {{0, 3}}, {}},
        {"filter.glare", {{0, 0}, {1, 0.2f}, {2, 15}}, {}},
        {"filter.glare", {{0, 1}, {1, 0.3f}, {2, 20}, {4, 6}}, {}},
        {"filter.glare", {{0, 2}, {2, 12}, {1, 0.4f}}, {}},
        {"filter.sun_beams", {{0, 0.2f}, {1, 0.8f}, {2, 0.6f}}, {}},
        {"matte.box_mask", {{4, 30}, {5, 0}, {7, 1}}, {}},
        {"matte.ellipse_mask", {{0, 0.3f}, {4, -20}, {5, 0.4f}, {7, 3}}, {{1, 2}}},
        {"matte.radial_gradient", {{2, 1.2f}, {7, 2}}, {}},
        {"matte.linear_gradient", {{0, 0.1f}, {1, 0.9f}, {2, 0.8f}, {3, 0.1f}, {5, 1}}, {{1, 2}}},
        {"matte.channel_key", {{0, 4}, {3, 1}}, {}},
        {"matte.channel_key", {{0, 7}}, {}},
        {"matte.channel_key", {{0, 0}, {1, 0.6f}, {2, 0.5f}}, {}},
        {"matte.luminance_key", {{2, 0}}, {}},
        {"matte.difference_key", {{1, 0.05f}}, {{1, 2}}},
        {"matte.distance_key", {{3, 1}}, {}},
        {"matte.distance_key", {{1, 0.02f}}, {{1, 0}}},
        {"matte.chroma_key", {{1, 10}, {3, 0.05f}}, {}},
        {"matte.color_spill", {{1, 0}, {2, 1}, {3, 0.8f}}, {{1, 1}}},
        {"tex.noise", {{0, 9}, {1, 3.5f}, {2, 0.7f}, {3, 2.5f}, {4, 1.5f}, {5, 7}}, {}},
        {"tex.voronoi", {{0, 23}, {1, 0.6f}, {2, 3}}, {}},
        {"tex.gradient", {{0, 1}, {1, 30}}, {}},
        {"tex.gradient", {{0, 2}, {1, -60}}, {}},
        {"tex.gradient", {{0, 3}}, {}},
        {"tex.gradient", {{0, 4}}, {}},
        {"tex.gradient", {{0, 5}}, {}},
        {"tex.gradient", {{0, 6}}, {}},
        {"tex.wave", {{0, 1}, {1, 1}, {2, 7}}, {}},
        {"tex.wave", {{0, 1}, {1, 2}, {2, 7}, {4, 2}}, {}},
        {"tex.wave", {{1, 2}, {2, 11}, {3, 35}, {6, 0.3f}}, {}},
        {"tex.checker", {{0, 13}}, {}},
        {"tex.white_noise", {{0, 3}, {1, 5}}, {}},
    };
    for (const Case& c : cases) {
        for (bool linear : {false, true}) {
            const std::string type = c.type;
            CAPTURE(type);
            CAPTURE(c.params.size());
            CAPTURE(linear);
            Graph g;
            Node* src = cpuSource(g);
            Node* split = g.addNode("color.split_rgb");
            g.connect(src->id, 0, split->id, 0);
            Node* n = g.addNode(c.type);
            const NodeInfo& inf = n->info();
            for (const auto& [i, v] : c.params) {
                const ParamKind k = inf.params[size_t(i)].kind;
                if (k == ParamKind::Bool) n->params[size_t(i)] = v != 0.0f;
                else if (k == ParamKind::Enum) n->params[size_t(i)] = int(v);
                else n->params[size_t(i)] = v;
            }
            // The first pin takes the source (a channel for mask nodes).
            if (inf.inputs.empty()) {
            } else if (inf.inputs[0].type == PinType::Image) {
                g.connect(src->id, 0, n->id, 0);
            } else {
                g.connect(split->id, 1, n->id, 0);
            }
            for (const auto& [pin, from] : c.wires) {
                if (from < 0) g.connect(src->id, 0, n->id, pin);
                else g.connect(split->id, from, n->id, pin);
            }
            for (int o = 0; o < int(inf.outputs.size()); ++o) {
                CAPTURE(o);
                // Textures also at a larger size: many more cell edges, where coordinates must match exactly.
                for (int sz : {1, inf.inputs.empty() ? 7 : 1}) {
                    const Run cpu = evaluate(g, n->id, o, false, linear, 61 * sz, 43 * sz);
                    const Run gpu = evaluate(g, n->id, o, true, linear, 61 * sz, 43 * sz);
                    CHECK(gpu.onGpu);
                    CHECK_MESSAGE(gpu.fallbacks == 0, gpu.error);
                    CAPTURE(sz);
                    CHECK(difference(cpu.v, gpu.v) < 2e-4f);
                }
            }
        }
    }
}

TEST_CASE("GPU normalize selects the same percentiles as the CPU") {
    if (!gpuReady()) return;
    // The device's radix select is exact, so on a channel (no luminance rounding) the result is
    // the CPU's to the rescale's float rounding. Ties and negatives exercise the key order; the
    // large size has many pixels per thread.
    struct Size {
        int w, h;
    } sizes[] = {{37, 23}, {1531, 1023}};
    const char* reds[] = {"1.3 * u - 0.1", "floor(u * 5) - 2"};
    const float pcts[][2] = {{0, 100}, {2, 98}, {50, 50}, {99.9f, 0.1f}};
    for (const Size& s : sizes) {
        for (const char* red : reds) {
            for (const auto& pct : pcts) {
                for (bool channel : {true, false}) {
                    // Equal percentiles divide by 1e-9, which magnifies any luminance rounding.
                    if (!channel && pct[0] == pct[1]) continue;
                    CAPTURE(s.w);
                    CAPTURE(red);
                    CAPTURE(pct[0]);
                    CAPTURE(pct[1]);
                    CAPTURE(channel);
                    Graph g;
                    Node* src = cpuSource(g);
                    src->params[0] = std::string(red) + " + 0 * fac(0 * x)";
                    Node* n = g.addNode("conv.normalize");
                    n->params[0] = pct[0];
                    n->params[1] = pct[1];
                    if (channel) {
                        Node* split = g.addNode("color.split_rgb");
                        g.connect(src->id, 0, split->id, 0);
                        g.connect(split->id, 0, n->id, 0);
                    } else {
                        g.connect(src->id, 0, n->id, 0);
                    }
                    const Run cpu = evaluate(g, n->id, 0, false, true, s.w, s.h);
                    const Run gpu = evaluate(g, n->id, 0, true, true, s.w, s.h);
                    REQUIRE(gpu.onGpu);
                    CHECK(gpu.fallbacks == 0);
                    // Luminance may round differently on the device, which can move a percentile
                    // by a neighbouring value; the source is smooth, so that stays small.
                    CHECK(difference(cpu.v, gpu.v) < (channel ? 1e-5f : 2e-3f));
                }
            }
        }
    }
}

TEST_CASE("GPU fusion runs chains of point ops as one shader") {
    if (!gpuReady()) return;
    // Every node type twice in a row (the same names in two stages of one shader), fed from the
    // CPU: the first is pending when the second runs, so it compiles into the second's shader.
    const int before = gpu::fusedStages();
    int chains = 0;
    for (const auto& type : NodeRegistry::instance().types()) {
        const NodeInfo* inf = NodeRegistry::instance().find(type);
        if (inf->hidden || inf->inputs.empty() || inf->outputs.empty()) continue;
        for (bool linear : {false, true}) {
            CAPTURE(type);
            CAPTURE(linear);
            Graph g;
            Node* src = cpuSource(g);
            Node* split = g.addNode("color.split_rgb");
            g.connect(src->id, 0, split->id, 0);
            Node* a = g.addNode(type);
            Node* b = g.addNode(type);
            for (int i = 0; i < int(inf->inputs.size()); ++i) {
                if (inf->inputs[i].type == PinType::Image) g.connect(src->id, 0, a->id, i);
                else g.connect(split->id, i % 3, a->id, i);
            }
            g.connect(a->id, 0, b->id, 0);
            // A hard step on a smooth input flips where the CPU and GPU round either side of it.
            if (type == "conv.threshold") b->params[2] = 0.25f;
            const Run cpu = evaluate(g, b->id, 0, false, linear), gpu = evaluate(g, b->id, 0, true, linear);
            CHECK_MESSAGE(gpu.fallbacks == 0, gpu.error);
            if (!gpu.onGpu) continue;
            ++chains;
            CHECK(cpu.v.empty() == gpu.v.empty());
            if (!cpu.v.empty() && !gpu.v.empty()) CHECK(difference(cpu.v, gpu.v) < 1e-3f);
        }
    }
    MESSAGE(chains << " chains on the GPU, " << gpu::fusedStages() - before << " stages fused");
    CHECK(gpu::fusedStages() - before >= chains);
}

TEST_CASE("GPU fusion keeps shared values, lookup tables and big chains right") {
    if (!gpuReady()) return;
    const nlohmann::json bent = {{0.0f, 0.1f}, {0.3f, 0.6f}, {0.7f, 0.5f}, {1.0f, 0.95f}};
    const nlohmann::json hue = {{0.0f, 0.3f}, {0.4f, 0.8f}, {0.8f, 0.45f}, {1.0f, 0.3f}};
    SUBCASE("a value read by two nodes runs once, and both see it") {
        // src -> gamma -> (invert, hue shift) -> mix: gamma has two readers.
        Graph g;
        Node* src = cpuSource(g);
        Node* gamma = g.addNode("color.gamma");
        Node* inv = g.addNode("color.invert");
        Node* hs = g.addNode("color.hue_shift");
        Node* mix = g.addNode("math.mix");
        g.connect(src->id, 0, gamma->id, 0);
        g.connect(gamma->id, 0, inv->id, 0);
        g.connect(gamma->id, 0, hs->id, 0);
        g.connect(inv->id, 0, mix->id, 0);
        g.connect(hs->id, 0, mix->id, 1);
        for (bool linear : {false, true}) {
            const Run cpu = evaluate(g, mix->id, 0, false, linear), gpu = evaluate(g, mix->id, 0, true, linear);
            REQUIRE(gpu.onGpu);
            CHECK(gpu.fallbacks == 0);
            CHECK(difference(cpu.v, gpu.v) < 1e-4f);
        }
    }
    SUBCASE("lookup tables of several stages") {
        Graph g;
        Node* src = cpuSource(g);
        Node* c1 = g.addNode("color.curves");
        Node* hc = g.addNode("color.hue_correct");
        Node* c2 = g.addNode("color.curves");
        c1->params[1] = nlohmann::json{{"master", bent}, {"r", hue}};
        hc->params[1] = nlohmann::json{{"h", hue}, {"s", bent}};
        c2->params[1] = nlohmann::json{{"g", hue}, {"b", bent}};
        g.connect(src->id, 0, c1->id, 0);
        g.connect(c1->id, 0, hc->id, 0);
        g.connect(hc->id, 0, c2->id, 0);
        const int before = gpu::fusedStages();
        const Run cpu = evaluate(g, c2->id, 0, false, true), gpu = evaluate(g, c2->id, 0, true, true);
        REQUIRE(gpu.onGpu);
        CHECK(gpu::fusedStages() - before == 2);
        CHECK(difference(cpu.v, gpu.v) < 1e-4f);
    }
    SUBCASE("more textures than one shader can read") {
        // 24 mixes in a row, each blending in its own CPU image: the whole chain would read 25
        // textures, so it splits into shaders the driver accepts.
        Graph g;
        Node* prev = cpuSource(g);
        for (int k = 0; k < 24; ++k) {
            Node* other = cpuSource(g);
            other->params[0] = "u * " + std::to_string(k + 1) + " * 0.04 + 0 * fac(0 * x)";
            Node* mix = g.addNode("math.mix");
            g.connect(prev->id, 0, mix->id, 0);
            g.connect(other->id, 0, mix->id, 1);
            prev = mix;
        }
        const Run cpu = evaluate(g, prev->id, 0, false, true), gpu = evaluate(g, prev->id, 0, true, true);
        REQUIRE(gpu.onGpu);
        CHECK(gpu.fallbacks == 0);
        CHECK(difference(cpu.v, gpu.v) < 1e-4f);
    }
}

TEST_CASE("GPU values pass through Reroute and Switch on the device") {
    if (!gpuReady()) return;
    Graph g;
    Node* src = cpuSource(g);
    Node* a = g.addNode("color.invert");
    Node* r = g.addNode("util.reroute");
    Node* sw = g.addNode("util.switch");
    Node* b = g.addNode("color.invert");
    g.connect(src->id, 0, a->id, 0);
    g.connect(a->id, 0, r->id, 0);
    g.connect(r->id, 0, sw->id, 1);
    sw->params[0] = true;
    g.connect(sw->id, 0, b->id, 0);
    const Run cpu = evaluate(g, b->id, 0, false, true), gpu = evaluate(g, b->id, 0, true, true);
    CHECK(difference(cpu.v, gpu.v) < 1e-5f);
    EvalContext ctx;
    ctx.defaultW = 37;
    ctx.defaultH = 23;
    ctx.gpu = true;
    ctx.colorManagement = g.colorManagement;
    Evaluator ev;
    ev.evaluateOutput(g, b->id, 0, ctx);
    CHECK(ev.gpuNodes()[r->id]);
    CHECK(ev.gpuNodes()[sw->id]);
    // A CPU value stays on the CPU (no upload just to pass it along).
    g.connect(src->id, 0, r->id, 0);
    Evaluator ev2;
    ev2.evaluateOutput(g, sw->id, 0, ctx);
    CHECK_FALSE(ev2.gpuNodes()[r->id]);
}

TEST_CASE("GPU display matches the CPU view transform, bytes and histogram") {
    if (!gpuReady()) return;
    // 211x97 with values from below 0 to well above 1, so every view's clipping and the AgX
    // shoulder are crossed.
    const int w = 211, h = 97;
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img->pixel(size_t(y) * w + x);
            const float u = float(x) / w, v = float(y) / h;
            p[0] = 4.0f * u * u - 0.05f, p[1] = 1.5f * v * (0.5f + 0.5f * std::sin(u * 11)), p[2] = 0.3f + 2.0f * u * v;
            p[3] = 1.0f;
        }
    std::vector<ColorManagement> cms(6, ColorManagement::sceneLinear());
    cms[0].linear = false;
    cms[2].view = ColorManagement::AgX;
    cms[3].view = ColorManagement::AgX, cms[3].look = ColorManagement::Punchy, cms[3].exposure = 0.7f;
    cms[4].view = ColorManagement::AgX, cms[4].look = ColorManagement::Greyscale, cms[4].gamma = 1.4f;
    cms[5].view = ColorManagement::Raw, cms[5].exposure = -1.3f, cms[5].gamma = 0.8f;
    auto byte = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    for (size_t ci = 0; ci < cms.size(); ++ci)
        for (bool clipping : {false, true}) {
            CAPTURE(ci);
            CAPTURE(clipping);
            gpu::DisplayResult r;
            {
                gpu::Scope scope;
                r = gpu::display(Value(img), cms[ci], clipping, true);
            }
            REQUIRE(r.bytes.size() == size_t(w) * h * 4);
            REQUIRE(r.histogram.size() == 1026);
            const ImagePtr disp = colormgmt::displayImage(img, cms[ci]);
            std::vector<uint32_t> hist(1026, 0);
            int worst = 0, off = 0;
            for (size_t i = 0; i < size_t(w) * h; ++i) {
                const float* d = disp->pixel(i);
                int b[3] = {byte(d[0]), byte(d[1]), byte(d[2])};
                ++hist[size_t(b[0])], ++hist[256 + size_t(b[1])], ++hist[512 + size_t(b[2])];
                ++hist[768 + size_t(byte(0.2126f * d[0] + 0.7152f * d[1] + 0.0722f * d[2]))];
                if (b[0] == 255 || b[1] == 255 || b[2] == 255) hist[1024] = 1;
                if (b[0] == 0 && b[1] == 0 && b[2] == 0) hist[1025] = 1;
                if (clipping) {
                    const int mx = std::max({b[0], b[1], b[2]});
                    if (mx == 255) b[0] = 255, b[1] = 0, b[2] = 0;
                    else if (mx == 0) b[0] = 0, b[1] = 90, b[2] = 255;
                }
                for (int k = 0; k < 3; ++k) {
                    const int dk = std::abs(b[k] - int(r.bytes[i * 4 + size_t(k)]));
                    worst = std::max(worst, dk);
                    off += dk != 0;
                }
                CHECK(r.bytes[i * 4 + 3] == 255);
            }
            // Float pow/log2 on the GPU can round a value to the neighbouring byte, and a
            // clipping colour can flip on a value that rounds to 0 or 255 on one side only.
            CHECK(off <= w * h / 200);
            if (!clipping) CHECK(worst <= 1);
            size_t binDiff = 0;
            for (size_t k = 0; k < 1024; ++k) binDiff += size_t(std::abs(int(hist[k]) - int(r.histogram[k])));
            CHECK(binDiff <= size_t(w * h / 50));
            if (ci == 0) {
                // Legacy values pass through, so everything is exact.
                CHECK(off == 0);
                CHECK(binDiff == 0);
            }
            CHECK(hist[1024] == r.histogram[1024]);
            CHECK(hist[1025] == r.histogram[1025]);
        }
}

TEST_CASE("GPU blur matches the CPU on long lines") {
    if (!gpuReady()) return;
    // Long lines along both axes, with radii from a pixel to a sizeable part of the line, so
    // windows clamp at both ends.
    for (auto [w, h] : {std::pair{2100, 37}, std::pair{37, 2100}})
        for (float sigma : {1.0f, 9.0f, 60.0f, 160.0f})
            for (bool channel : {false, true}) {
                CAPTURE(w);
                CAPTURE(sigma);
                CAPTURE(channel);
                auto img = std::make_shared<Image>(w, h);
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        float* p = img->pixel(size_t(y) * w + x);
                        p[0] = float((x * 7 + y * 13) % 97) / 40.0f;  // rough, so every tap matters
                        p[1] = 0.5f + 0.5f * std::sin(x * 0.01f + y * 0.3f);
                        p[2] = float(x % 2), p[3] = 1.0f;
                    }
                const float sx = w > h ? sigma : 0.6f * sigma, sy = w > h ? 0.6f * sigma : sigma;
                Value got;
                Value want;
                if (channel) {
                    auto c = toChannel(Value(img));
                    std::vector<float> data = c->data;
                    imageops::blurChannel(data, w, h, sx, sy);
                    auto wc = std::make_shared<Channel>(*c);
                    wc->data = data;
                    want = Value(ChannelPtr(wc));
                    gpu::Scope scope;
                    auto up = gpu::upload(*c);
                    auto r = std::make_shared<GpuChannel>();
                    r->w = w, r->h = h, r->tex = gpu::boxBlur(up->texture(), sx, sy);
                    got = toCpu(Value(GpuChannelPtr(r)));
                } else {
                    auto cpu = std::make_shared<Image>(*img);
                    imageops::blurImage(*cpu, sx, sy);
                    want = Value(ImagePtr(cpu));
                    gpu::Scope scope;
                    auto up = gpu::upload(*img, gpu::Format::RGBA32F);
                    auto r = std::make_shared<GpuImage>();
                    r->w = w, r->h = h, r->tex = gpu::boxBlur(up->texture(), sx, sy);
                    got = toCpu(Value(GpuImagePtr(r)));
                }
                CHECK(difference(want, got) < 2e-4f);
            }
}

// File > Export with the GPU device on: a full-resolution render on the device, at Full
// precision, writes what the CPU writes.
TEST_CASE("GPU export matches the CPU export") {
    if (!gpuReady()) return;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "nodelab_gpu_export";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Graph g;
    Node* src = source(g);
    Node* basic = g.addNode("color.basic");
    basic->params[9] = 50.0f;
    basic->params[10] = 60.0f;
    Node* blur = g.addNode("filter.blur");
    Node* out = g.addNode("io.output");
    g.connect(src->id, 0, basic->id, 0);
    g.connect(basic->id, 0, blur->id, 0);
    g.connect(blur->id, 0, out->id, 0);
    ExportSettings s;
    s.depth = 16;
    s.fileOutputs = false;
    std::string err;
    ImagePtr res[2];
    for (int k = 0; k < 2; ++k) {
        const std::string path = pathToU8(dir / (k ? "gpu.png" : "cpu.png"));
        Exporter ex;
        ex.start(g.toJson(), {ExportItem{"", path}}, 0, s, k == 1);
        ex.wait();
        for (const std::string& line : ex.takeLog()) CHECK_MESSAGE(line.find("CPU instead") == std::string::npos, line);
        CHECK(ex.progress().failed == 0);
        res[k] = loadImage(path, err);
        REQUIRE(res[k]);
    }
    REQUIRE(res[0]->w == res[1]->w);
    REQUIRE(res[0]->h == res[1]->h);
    CHECK(difference(Value(res[0]), Value(res[1])) < 3e-4f);
    fs::remove_all(dir);
}

TEST_CASE("GPU nodes match the CPU on one-pixel-wide images") {
    if (!gpuReady()) return;
    const int sizes[][2] = {{1, 1}, {1, 5}, {5, 1}};
    for (const auto& type : NodeRegistry::instance().types()) {
        const NodeInfo* inf = NodeRegistry::instance().find(type);
        if (inf->hidden) continue;
        for (const auto& sz : sizes) {
            for (bool linear : {false, true}) {
                Graph g;
                Node* src = cpuSource(g);
                Node* split = g.addNode("color.split_rgb");
                g.connect(src->id, 0, split->id, 0);
                Node* n = g.addNode(type);
                for (int i = 0; i < int(inf->inputs.size()); ++i) {
                    if (inf->inputs[i].type == PinType::Image) g.connect(src->id, 0, n->id, i);
                    else g.connect(split->id, i % 3, n->id, i);
                }
                for (int o = 0; o < int(inf->outputs.size()); ++o) {
                    CAPTURE(type);
                    CAPTURE(sz[0]);
                    CAPTURE(sz[1]);
                    CAPTURE(linear);
                    CAPTURE(o);
                    const Run cpu = evaluate(g, n->id, o, false, linear, sz[0], sz[1]);
                    const Run gpu = evaluate(g, n->id, o, true, linear, sz[0], sz[1]);
                    CHECK_MESSAGE(gpu.fallbacks == 0, gpu.error);
                    if (!gpu.onGpu) continue;
                    CHECK(cpu.v.empty() == gpu.v.empty());
                    if (!cpu.v.empty() && !gpu.v.empty()) CHECK(difference(cpu.v, gpu.v) < 2e-4f);
                }
            }
        }
    }
}
