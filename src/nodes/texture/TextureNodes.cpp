// Procedural textures. Coordinates are normalized to the image (x across scaled by aspect, y
// down), so a texture looks the same in the preview and in the full-resolution export.
#include <cmath>

#include "core/Noise.h"
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

// Textures are generated per pixel from its position, so a region renders just its window.
class TextureBase : public Node {
public:
    int roiPadding(const EvalContext&) const override { return 0; }
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
