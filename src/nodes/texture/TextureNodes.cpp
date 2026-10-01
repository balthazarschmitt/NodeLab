// Procedural textures. Coordinates are normalized to the image (x across scaled by aspect, y
// down), so a texture looks the same in the preview and in the full-resolution export.
#include <cmath>

#include "core/Noise.h"
#include "gpu/PointOp.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

constexpr float kPi = 3.14159265f;

// Fills a Fac channel and a Color image of the context size from fn(u, v, x, y, fac, rgb), with
// x, y in the full image's pixels (a region renders only its window).
template <typename Fn>
void texture(EvalContext& ctx, std::vector<Value>& out, int facPin, int colorPin, Fn&& fn) {
    int bw, bh;
    resolveSize({}, ctx, bw, bh);
    bw = std::max(1, bw);
    bh = std::max(1, bh);
    const PixelFrame fr = frameOf(ctx, bw, bh);
    const int w = std::max(1, fr.fullW), h = std::max(1, fr.fullH);
    const float aspect = float(w) / h;
    auto fac = std::make_shared<Channel>(Channel::makeSized(bw, bh));
    auto img = std::make_shared<Image>(bw, bh);
    parallelFor(bh, [&](int by) {
        for (int bx = 0; bx < bw; ++bx) {
            size_t i = size_t(by) * bw + bx;
            const int x = bx + fr.x0, y = by + fr.y0;
            float u = ((x + 0.5f) / w - 0.5f) * aspect, v = (y + 0.5f) / h - 0.5f;
            float f = 0, c[3] = {0, 0, 0};
            fn(u, v, x, y, f, c);
            fac->data[i] = f;
            float* d = img->pixel(i);
            d[0] = c[0], d[1] = c[1], d[2] = c[2], d[3] = 1.0f;
        }
    });
    if (facPin >= 0) out[facPin] = Value(ChannelPtr(fac));
    if (colorPin >= 0) out[colorPin] = Value(ImagePtr(img));
}

// core/Noise.h in GLSL, bit for bit in the hashes (lattice cells and cell colours must match).
const char* const kGlslNoise = R"(
uint nHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
uint nHash2(int x, int y, uint seed) { return nHash(uint(x) * 0x9E3779B1u ^ nHash(uint(y) * 0x85EBCA77u ^ nHash(seed))); }
float nHashFloat(int x, int y, uint seed, uint salt) {
    return float(nHash2(x, y, seed + salt * 0x68E31DA4u) >> 8) * (1.0 / 16777216.0);
}
float nGrad(int ix, int iy, float dx, float dy, uint seed) {
    float a = float(nHash2(ix, iy, seed) & 0xFFFFu) * (1.0 / 65536.0) * 6.2831853;
    return cos(a) * dx + sin(a) * dy;
}
float nFade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
float nPerlin(float x, float y, uint seed) {
    float xf = floor(x), yf = floor(y);
    int x0 = int(xf), y0 = int(yf);
    float fx = x - xf, fy = y - yf;
    float u = nFade(fx), v = nFade(fy);
    float n00 = nGrad(x0, y0, fx, fy, seed), n10 = nGrad(x0 + 1, y0, fx - 1.0, fy, seed);
    float n01 = nGrad(x0, y0 + 1, fx, fy - 1.0, seed), n11 = nGrad(x0 + 1, y0 + 1, fx - 1.0, fy - 1.0, seed);
    float nx0 = n00 + (n10 - n00) * u, nx1 = n01 + (n11 - n01) * u;
    return (nx0 + (nx1 - nx0) * v) * 1.414;
}
float nFbm(float x, float y, float detail, float roughness, float lacunarity, uint seed) {
    float sum = 0.0, amp = 1.0, norm = 0.0, freq = 1.0;
    int octaves = int(detail);
    for (int i = 0; i <= octaves; ++i) {
        sum += nPerlin(x * freq, y * freq, seed + uint(i) * 131u) * amp;
        norm += amp;
        amp *= roughness;
        freq *= lacunarity;
    }
    float frac = detail - float(octaves);
    if (frac > 0.0) {
        sum += nPerlin(x * freq, y * freq, seed + uint(octaves + 1) * 131u) * amp * frac;
        norm += amp * frac;
    }
    return 0.5 + 0.5 * sum / norm;
}
float nVoronoi(float x, float y, float randomness, uint seed, out ivec2 cell) {
    int xi = int(floor(x)), yi = int(floor(y));
    float best = 1e9;
    cell = ivec2(xi, yi);
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            int cx = xi + i, cy = yi + j;
            precise float px = float(cx) + 0.5 + (nHashFloat(cx, cy, seed, 1u) - 0.5) * randomness;
            precise float py = float(cy) + 0.5 + (nHashFloat(cx, cy, seed, 2u) - 0.5) * randomness;
            float d = length(vec2(px - x, py - y));
            if (d < best) {
                best = d;
                cell = ivec2(cx, cy);
            }
        }
    return best;
}
)";

// std::hypot, correctly rounded through double precision: a float length() an ulp off moves the
// edges of the Rings saw bands by a pixel. (Voronoi compares distances, where an ulp is harmless.)
// Only Rings uses it, so a device without fp64 loses only that mode (to the CPU fallback).
const char* const kGlslHypot =
    "float nHypot(float x, float y) { return float(sqrt(double(x) * double(x) + double(y) * double(y))); }\n";

// Textures are generated per pixel from its position, so a region renders just its window.
class TextureBase : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }
    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; }

protected:
    // The GPU version of texture(): the body sets out<k> from u, v (as texture()'s) and xy, the
    // pixel in the full image. P[0] is the aspect; the node's own params follow from P[1].
    // u and v come from the CPU, one per column and row (in lut, followed by `lut`): GPU division
    // isn't correctly rounded, and an ulp moves the edges of cells, checkers and bands by a pixel.
    void runGpu(EvalContext& ctx, std::vector<Value>& out, const std::string& body, std::vector<float> params,
                const std::string& functions = "", const std::vector<float>& lut = {}) {
        gpu::PointOp g;
        resolveSize({}, ctx, g.w, g.h);
        g.w = std::max(1, g.w);
        g.h = std::max(1, g.h);
        const PixelFrame fr = frameOf(ctx, g.w, g.h);
        const int w = std::max(1, fr.fullW), h = std::max(1, fr.fullH);
        const float aspect = float(w) / h;
        g.lut.resize(size_t(g.w) + g.h);
        for (int bx = 0; bx < g.w; ++bx) g.lut[size_t(bx)] = ((bx + fr.x0 + 0.5f) / w - 0.5f) * aspect;
        for (int by = 0; by < g.h; ++by) g.lut[size_t(g.w) + by] = (by + fr.y0 + 0.5f) / h - 0.5f;
        g.lut.insert(g.lut.end(), lut.begin(), lut.end());
        g.functions = kGlslNoise + functions;
        g.body = R"(
    ivec2 xy = p + uOrigin;
    float u = lutAt(p.x), v = lutAt(uSize.x + p.y);
)" + body;
        g.params = {aspect};
        g.params.insert(g.params.end(), params.begin(), params.end());
        gpu::runPoint(ctx, *this, g, {}, out);
    }
};

uint32_t seedOf(const Node& n, int param) { return uint32_t(std::max(0, int(std::round(n.paramF(param))))); }

class NoiseTextureNode : public TextureBase {
public:
    NODELAB_NODE({"tex.noise", "Noise Texture", "Texture",
                  {},
                  {{"Fac", PinType::Channel}, {"Color", PinType::Image}},
                  {ParamDesc::Float("Scale", 5.0f, 0.1f, 50.0f), ParamDesc::Float("Detail", 2.0f, 0.0f, 12.0f),
                   ParamDesc::Float("Roughness", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Lacunarity", 2.0f, 1.0f, 4.0f),
                   ParamDesc::Float("Distortion", 0.0f, 0.0f, 5.0f), ParamDesc::Float("Seed", 0.0f, 0.0f, 100.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        const float scale = paramF(0), detail = paramF(1), rough = paramF(2), lac = paramF(3), dist = paramF(4);
        const uint32_t seed = seedOf(*this, 5);
        texture(ctx, out, 0, 1, [&](float u, float v, int, int, float& f, float* c) {
            float x = u * scale, y = v * scale;
            if (dist > 0) {  // domain warp
                x += (noise::fbm(x + 13.1f, y, detail, rough, lac, seed + 7) - 0.5f) * dist;
                y += (noise::fbm(x, y + 7.7f, detail, rough, lac, seed + 11) - 0.5f) * dist;
            }
            f = noise::fbm(x, y, detail, rough, lac, seed);
            c[0] = f;
            c[1] = noise::fbm(x, y, detail, rough, lac, seed + 101);
            c[2] = noise::fbm(x, y, detail, rough, lac, seed + 202);
        });
    }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        runGpu(ctx, out, R"(
    precise float x = u * P[1], y = v * P[1];
    uint seed = uint(P[6]);
    if (P[5] > 0.0) {
        x += (nFbm(x + 13.1, y, P[2], P[3], P[4], seed + 7u) - 0.5) * P[5];
        y += (nFbm(x, y + 7.7, P[2], P[3], P[4], seed + 11u) - 0.5) * P[5];
    }
    float f = nFbm(x, y, P[2], P[3], P[4], seed);
    out0 = f;
    out1 = vec4(f, nFbm(x, y, P[2], P[3], P[4], seed + 101u), nFbm(x, y, P[2], P[3], P[4], seed + 202u), 1.0);
)",
               {paramF(0), paramF(1), paramF(2), paramF(3), paramF(4), float(seedOf(*this, 5))});
    }
};

class VoronoiTextureNode : public TextureBase {
public:
    NODELAB_NODE({"tex.voronoi", "Voronoi Texture", "Texture",
                  {},
                  {{"Distance", PinType::Channel}, {"Color", PinType::Image}},
                  {ParamDesc::Float("Scale", 8.0f, 0.1f, 60.0f), ParamDesc::Float("Randomness", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Seed", 0.0f, 0.0f, 100.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        const float scale = paramF(0), rnd = paramF(1);
        const uint32_t seed = seedOf(*this, 2);
        texture(ctx, out, 0, 1, [&](float u, float v, int, int, float& f, float* c) {
            int cx, cy;
            f = std::min(1.0f, noise::voronoi(u * scale, v * scale, rnd, seed, cx, cy));
            for (int k = 0; k < 3; ++k) c[k] = noise::hashFloat(cx, cy, seed, 10 + k);
        });
    }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        runGpu(ctx, out, R"(
    uint seed = uint(P[3]);
    ivec2 cell;
    out0 = min(1.0, nVoronoi(u * P[1], v * P[1], P[2], seed, cell));
    out1 = vec4(nHashFloat(cell.x, cell.y, seed, 10u), nHashFloat(cell.x, cell.y, seed, 11u),
                nHashFloat(cell.x, cell.y, seed, 12u), 1.0);
)",
               {paramF(0), paramF(1), float(seedOf(*this, 2))});
    }
};

class GradientTextureNode : public TextureBase {
public:
    enum { Linear, Quadratic, Easing, Diagonal, Spherical, QuadraticSphere, Radial };
    NODELAB_NODE({"tex.gradient", "Gradient Texture", "Texture",
                  {},
                  {{"Fac", PinType::Channel}, {"Color", PinType::Image}},
                  {ParamDesc::Enum("Type", Linear, {"Linear", "Quadratic", "Easing", "Diagonal", "Spherical",
                                                     "Quadratic Sphere", "Radial"}),
                   ParamDesc::Float("Angle", 0.0f, -180.0f, 180.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        const int type = paramI(0);
        const float a = paramF(1) * kPi / 180.0f, ca = std::cos(a), sa = std::sin(a);
        const float aspect = float(std::max(1, ctx.roi ? ctx.roi->canvasW : ctx.defaultW)) /
                             std::max(1, ctx.roi ? ctx.roi->canvasH : ctx.defaultH);
        texture(ctx, out, 0, 1, [&](float u, float v, int, int, float& f, float* c) {
            float ru = u * ca + v * sa;                       // rotated axis, spans about -aspect/2..aspect/2
            float t = ru / std::max(aspect, 1e-3f) + 0.5f;    // 0..1 across the image at angle 0
            float r = std::hypot(u, v) * 2.0f;                // 0 at the center, ~1 at the edges
            switch (type) {
                case Quadratic: f = std::clamp(t, 0.0f, 1.0f); f *= f; break;
                case Easing: f = std::clamp(t, 0.0f, 1.0f); f = f * f * (3 - 2 * f); break;
                case Diagonal: f = ((u / aspect + 0.5f) + (v + 0.5f)) * 0.5f; break;
                case Spherical: f = std::max(0.0f, 1.0f - r); break;
                case QuadraticSphere: f = std::max(0.0f, 1.0f - r); f *= f; break;
                case Radial: f = std::atan2(v, u) / (2 * kPi) + 0.5f; break;
                default: f = t; break;
            }
            f = std::clamp(f, 0.0f, 1.0f);
            c[0] = c[1] = c[2] = f;
        });
    }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        const float a = paramF(1) * kPi / 180.0f;
        runGpu(ctx, out, R"(
    float ru = u * P[1] + v * P[2];
    float t = ru / max(P[0], 1e-3) + 0.5;
    float r = length(vec2(u, v)) * 2.0;
    float f;
    if (TYPE == 1) f = clamp01(t) * clamp01(t);
    else if (TYPE == 2) { f = clamp01(t); f = f * f * (3.0 - 2.0 * f); }
    else if (TYPE == 3) f = ((u / P[0] + 0.5) + (v + 0.5)) * 0.5;
    else if (TYPE == 4) f = max(0.0, 1.0 - r);
    else if (TYPE == 5) { f = max(0.0, 1.0 - r); f *= f; }
    else if (TYPE == 6) f = atan2C(v, u) / (2.0 * 3.14159265) + 0.5;
    else f = t;
    f = clamp01(f);
    out0 = f;
    out1 = vec4(f, f, f, 1.0);
)",
               {std::cos(a), std::sin(a)}, "const int TYPE = " + std::to_string(paramI(0)) + ";\n");
    }
};

class WaveTextureNode : public TextureBase {
public:
    NODELAB_NODE({"tex.wave", "Wave Texture", "Texture",
                  {},
                  {{"Fac", PinType::Channel}, {"Color", PinType::Image}},
                  {ParamDesc::Enum("Type", 0, {"Bands", "Rings"}), ParamDesc::Enum("Profile", 0, {"Sine", "Saw", "Triangle"}),
                   ParamDesc::Float("Scale", 5.0f, 0.1f, 50.0f), ParamDesc::Float("Angle", 0.0f, -180.0f, 180.0f),
                   ParamDesc::Float("Distortion", 0.0f, 0.0f, 20.0f), ParamDesc::Float("Detail", 2.0f, 0.0f, 12.0f),
                   ParamDesc::Float("Phase", 0.0f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        const int type = paramI(0), profile = paramI(1);
        const float scale = paramF(2), a = paramF(3) * kPi / 180.0f, dist = paramF(4), detail = paramF(5), phase = paramF(6);
        const float ca = std::cos(a), sa = std::sin(a);
        texture(ctx, out, 0, 1, [&](float u, float v, int, int, float& f, float* c) {
            float x = u * scale, y = v * scale;
            float n = type == 0 ? (x * ca + y * sa) : std::hypot(x, y);
            if (dist > 0) n += dist * (noise::fbm(x, y, detail, 0.5f, 2.0f, 3) - 0.5f);
            n = n + phase;
            switch (profile) {
                case 1: f = n - std::floor(n); break;                                 // saw
                case 2: f = 1.0f - std::fabs(2.0f * (n - std::floor(n)) - 1.0f); break;  // triangle
                default: f = 0.5f + 0.5f * std::sin(n * 2.0f * kPi); break;           // sine
            }
            c[0] = c[1] = c[2] = f;
        });
    }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        const float a = paramF(3) * kPi / 180.0f;
        runGpu(ctx, out, R"(
    precise float x = u * P[1], y = v * P[1];
    precise float n = RINGS ? nHypot(x, y) : x * P[2] + y * P[3];
    if (P[4] > 0.0) n += P[4] * (nFbm(x, y, P[5], 0.5, 2.0, 3u) - 0.5);
    n = n + P[6];
    float f;
    if (PROFILE == 1) f = n - floor(n);
    else if (PROFILE == 2) f = 1.0 - abs(2.0 * (n - floor(n)) - 1.0);
    else f = 0.5 + 0.5 * sin(n * 2.0 * 3.14159265);
    out0 = f;
    out1 = vec4(f, f, f, 1.0);
)",
               {paramF(2), std::cos(a), std::sin(a), paramF(4), paramF(5), paramF(6)},
               paramI(0) == 1 ? std::string(kGlslHypot) + "const bool RINGS = true;\nconst int PROFILE = " +
                                    std::to_string(paramI(1)) + ";\n"
                              : "float nHypot(float x, float y) { return 0.0; }\nconst bool RINGS = false;\nconst int PROFILE = " +
                                    std::to_string(paramI(1)) + ";\n");
    }
};

class CheckerTextureNode : public TextureBase {
public:
    NODELAB_NODE({"tex.checker", "Checker Texture", "Texture",
                  {},
                  {{"Color", PinType::Image}, {"Fac", PinType::Channel}},
                  {ParamDesc::Float("Scale", 8.0f, 0.5f, 100.0f), ParamDesc::Color("Color 1", 0.8f, 0.8f, 0.8f),
                   ParamDesc::Color("Color 2", 0.2f, 0.2f, 0.2f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        const float scale = paramF(0);
        float c1[3], c2[3];
        paramC(1, c1);
        paramC(2, c2);
        texture(ctx, out, 1, 0, [&](float u, float v, int, int, float& f, float* c) {
            int cell = int(std::floor(u * scale)) + int(std::floor(v * scale));
            f = (cell & 1) ? 0.0f : 1.0f;
            const float* src = f > 0.5f ? c1 : c2;
            c[0] = src[0], c[1] = src[1], c[2] = src[2];
        });
    }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        float c1[3], c2[3];
        paramC(1, c1);
        paramC(2, c2);
        runGpu(ctx, out, R"(
    int cell = int(floor(u * P[1])) + int(floor(v * P[1]));
    bool first = (cell & 1) == 0;
    out1 = first ? 1.0 : 0.0;
    out0 = vec4(first ? vec3(P[2], P[3], P[4]) : vec3(P[5], P[6], P[7]), 1.0);
)",
               {paramF(0), c1[0], c1[1], c1[2], c2[0], c2[1], c2[2]});
    }
};

class WhiteNoiseNode : public TextureBase {
public:
    NODELAB_NODE({"tex.white_noise", "White Noise", "Texture",
                  {},
                  {{"Value", PinType::Channel}, {"Color", PinType::Image}},
                  {ParamDesc::Float("Grain Size", 1.0f, 1.0f, 32.0f), ParamDesc::Float("Seed", 0.0f, 0.0f, 100.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        // Cells are in full-resolution pixels, so grain size matches between preview and export.
        const float cell = std::max(1.0f, paramF(0)) * ctx.scale;
        const uint32_t seed = seedOf(*this, 1);
        texture(ctx, out, 0, 1, [&](float, float, int x, int y, float& f, float* c) {
            int ix = int(std::floor(x / cell)), iy = int(std::floor(y / cell));
            f = noise::hashFloat(ix, iy, seed);
            for (int k = 0; k < 3; ++k) c[k] = noise::hashFloat(ix, iy, seed, 20 + k);
        });
    }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        // The cell of each column, then of each row, as the CPU computes them.
        int w, h;
        resolveSize({}, ctx, w, h);
        w = std::max(1, w);
        h = std::max(1, h);
        const PixelFrame fr = frameOf(ctx, w, h);
        const float cell = std::max(1.0f, paramF(0)) * ctx.scale;
        std::vector<float> cells(size_t(w) + h);
        for (int x = 0; x < w; ++x) cells[size_t(x)] = std::floor((x + fr.x0) / cell);
        for (int y = 0; y < h; ++y) cells[size_t(w) + y] = std::floor((y + fr.y0) / cell);
        runGpu(ctx, out, R"(
    uint seed = uint(P[1]);
    int ix = int(lutAt(uSize.x + uSize.y + p.x)), iy = int(lutAt(2 * uSize.x + uSize.y + p.y));
    out0 = nHashFloat(ix, iy, seed, 0u);
    out1 = vec4(nHashFloat(ix, iy, seed, 20u), nHashFloat(ix, iy, seed, 21u), nHashFloat(ix, iy, seed, 22u), 1.0);
)",
               {float(seedOf(*this, 1))}, "", cells);
    }
};

}  // namespace

void registerTextureNodes(NodeRegistry& r) {
    r.add<NoiseTextureNode>();
    r.add<VoronoiTextureNode>();
    r.add<GradientTextureNode>();
    r.add<WaveTextureNode>();
    r.add<CheckerTextureNode>();
    r.add<WhiteNoiseNode>();
}
