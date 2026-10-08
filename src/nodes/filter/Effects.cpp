// Lightroom's Effects and Lens Corrections panels: Post-Crop Vignetting, Defringe, and a Border
// (darktable's framing). Sizes are relative to the image (or in full-resolution pixels times
// ctx.scale), so the preview matches the export.
#include <algorithm>
#include <cmath>

#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "gpu/PointOp.h"
#include "graph/NodeRegistry.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

float smoothstepf(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / std::max(e1 - e0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------- Vignette

// Lightroom's Post-Crop Vignetting: darkens (or lightens) towards the frame of the image it gets,
// so after a Crop it follows the crop. Amount -100 is two stops darker at the edges.
class VignetteNode : public Node {
public:
    enum { Style, Amount, Midpoint, Roundness, Feather, Highlights };
    enum { HighlightPriority, ColorPriority, PaintOverlay };
    REFRACTORY_NODE({"filter.vignette", "Vignette", "Filter",
                  {{"Image", PinType::Image}, {"Amount", PinType::Channel, Amount}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Enum("Style", HighlightPriority, {"Highlight Priority", "Color Priority", "Paint Overlay"}),
                   ParamDesc::Float("Amount", -25.0f, -100.0f, 100.0f), ParamDesc::Float("Midpoint", 50.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Roundness", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Feather", 50.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Highlights", 0.0f, 0.0f, 100.0f)}})
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel, at global positions
    bool paramHidden(int i) const override { return i == Highlights && paramI(Style) == PaintOverlay; }

    // The shape, shared with the GPU: radii, exponent, and the falloff's middle and width in units
    // of the distance to the frame's edge.
    struct Shape {
        float rx, ry, n, mid, width;
    };
    Shape shape(int fullW, int fullH) const {
        const float hw = fullW * 0.5f, hh = fullH * 0.5f, m = std::min(hw, hh);
        const float r = paramF(Roundness) / 100.0f, t = std::max(r, 0.0f);
        // Roundness 100 is a circle, 0 an ellipse fitting the frame, -100 a rounded rectangle.
        return {std::max(hw + (m - hw) * t, 1e-3f), std::max(hh + (m - hh) * t, 1e-3f), 2.0f + std::max(-r, 0.0f) * 6.0f,
                0.35f + 0.9f * paramF(Midpoint) / 100.0f, 0.05f + 1.1f * paramF(Feather) / 100.0f};
    }
    static float maskAt(const Shape& s, float dx, float dy) {
        const float d = std::pow(std::pow(std::fabs(dx) / s.rx, s.n) + std::pow(std::fabs(dy) / s.ry, s.n), 1.0f / s.n);
        return smoothstepf(s.mid - s.width * 0.5f, s.mid + s.width * 0.5f, d);
    }

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const PixelFrame f = frameOf(ctx, src->w, src->h);
        const Shape sh = shape(f.fullW, f.fullH);
        ChannelPtr am = channelOr(in[1], paramF(Amount));
        ChannelSampler sa = paramSampler(*this, 1, am, src->w, src->h);
        const int style = paramI(Style);
        const float hl = paramF(Highlights) / 100.0f;
        const bool lin = ctx.linear();
        using colormath::linearToSrgb, colormath::srgbToLinear;
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            const float a = std::clamp(sa(x, y) / 100.0f, -1.0f, 1.0f);
            const float m = maskAt(sh, f.x0 + x + 0.5f - f.fullW * 0.5f, f.y0 + y + 0.5f - f.fullH * 0.5f);
            if (style == PaintOverlay) {
                // Towards black or white in display values, as Lightroom's old style.
                const float target = a < 0.0f ? 0.0f : 1.0f, t = std::fabs(a) * m;
                for (int k = 0; k < 3; ++k) {
                    const float e = lin ? linearToSrgb(s[k]) : s[k];
                    const float v = e + (target - e) * t;
                    d[k] = clampColor(lin, lin ? srgbToLinear(v) : v);
                }
            } else {
                float L[3];
                for (int k = 0; k < 3; ++k) L[k] = lin ? s[k] : srgbToLinear(s[k]);
                float kf = std::exp2(2.0f * a * m);
                if (a < 0.0f) {
                    // Highlights: bright parts (lamps, sky) keep their brightness in the darkened corners.
                    const float ld = linearToSrgb(std::clamp(luminance(L[0], L[1], L[2]), 0.0f, 1.0f));
                    kf += (1.0f - kf) * hl * smoothstepf(0.5f, 1.0f, ld);
                }
                for (int k = 0; k < 3; ++k) {
                    float v;
                    if (a > 0.0f && style == HighlightPriority) {
                        // Lightening like a screen in display values, so highlights don't clip.
                        const float e = linearToSrgb(L[k]), kd = std::pow(kf, 1.0f / 2.2f);
                        v = srgbToLinear(1.0f - (1.0f - e) / kd);
                    } else {
                        v = L[k] * kf;
                    }
                    d[k] = clampColor(lin, lin ? v : linearToSrgb(v));
                }
            }
            d[3] = s[3];
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w = 0, h = 0;
        in[0].size(w, h);
        const PixelFrame f = frameOf(ctx, w, h);
        const Shape sh = shape(f.fullW, f.fullH);
        gpu::PointOp op;
        op.params = {sh.rx, sh.ry, sh.n, sh.mid, sh.width, float(paramI(Style)), paramF(Highlights) / 100.0f};
        op.defaults = {NAN, paramF(Amount)};
        op.body = R"(
    vec4 s = img0(p);
    float a = clamp(par1(p) / 100.0, -1.0, 1.0);
    vec2 dd = abs(vec2(uOrigin + p) + 0.5 - vec2(uFull) * 0.5) / vec2(P[0], P[1]);
    float dist = pow(pow(dd.x, P[2]) + pow(dd.y, P[2]), 1.0 / P[2]);
    float m = smoothstep(P[3] - P[4] * 0.5, P[3] + P[4] * 0.5, dist);
    int style = int(P[5]);
    vec3 o;
    if (style == 2) {
        float target = a < 0.0 ? 0.0 : 1.0;
        vec3 e = uLinear ? linearToSrgb(s.rgb) : s.rgb;
        vec3 v = e + (target - e) * (abs(a) * m);
        o = clampColor(uLinear, uLinear ? srgbToLinear(v) : v);
    } else {
        vec3 L = uLinear ? s.rgb : srgbToLinear(s.rgb);
        float kf = exp2(2.0 * a * m);
        if (a < 0.0) {
            float ld = linearToSrgb(clamp(luminance(L), 0.0, 1.0));
            kf += (1.0 - kf) * P[6] * smoothstep(0.5, 1.0, ld);
        }
        vec3 v;
        if (a > 0.0 && style == 0) v = srgbToLinear(1.0 - (1.0 - linearToSrgb(L)) / pow(kf, 1.0 / 2.2));
        else v = L * kf;
        o = clampColor(uLinear, uLinear ? v : linearToSrgb(v));
    }
    out0 = vec4(o, s.a);
)";
        gpu::runOver(ctx, *this, op, in, out);
    }
};

// ---------------------------------------------------------------- Defringe

// Lightroom's Defringe: removes purple and green fringes (longitudinal chromatic aberration) along
// high-contrast edges. Pixels whose hue is in a fringe's range, near an edge, lose their colour
// (keeping their luminance). Amount sets the strength and how far from the edge it reaches.
class DefringeNode : public Node {
public:
    enum { PurpleAmount, PurpleLow, PurpleHigh, GreenAmount, GreenLow, GreenHigh };
    REFRACTORY_NODE({"filter.defringe", "Defringe", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Purple Amount", 0.0f, 0.0f, 20.0f), ParamDesc::Float("Purple Hue Low", 30.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Purple Hue High", 70.0f, 0.0f, 100.0f), ParamDesc::Float("Green Amount", 0.0f, 0.0f, 20.0f),
                   ParamDesc::Float("Green Hue Low", 40.0f, 0.0f, 100.0f), ParamDesc::Float("Green Hue High", 60.0f, 0.0f, 100.0f)}})

    // The hue sliders span Lightroom's ranges: purple from blue (230 degrees) to magenta (330),
    // green from yellow-green (60) to cyan-green (180). Low and high may be given either way round.
    struct Plan {
        float pStrength, pLo, pHi, gStrength, gLo, gHi;
        int radius;  // working pixels
    };
    Plan plan(const EvalContext& ctx) const {
        Plan p;
        const float pa = paramF(PurpleAmount), ga = paramF(GreenAmount);
        p.pStrength = std::min(1.0f, pa / 8.0f), p.gStrength = std::min(1.0f, ga / 8.0f);
        p.pLo = 230.0f + std::min(paramF(PurpleLow), paramF(PurpleHigh));
        p.pHi = 230.0f + std::max(paramF(PurpleLow), paramF(PurpleHigh));
        p.gLo = 60.0f + 1.2f * std::min(paramF(GreenLow), paramF(GreenHigh));
        p.gHi = 60.0f + 1.2f * std::max(paramF(GreenLow), paramF(GreenHigh));
        p.radius = std::max(1, int(std::ceil((1.0f + 3.0f * std::max(pa, ga) / 20.0f) * ctx.scale)));
        return p;
    }
    int roiPadding(const EvalContext& ctx) const override { return plan(ctx).radius; }

    static float hueWindow(float h, float lo, float hi) {
        return smoothstepf(lo - 10.0f, lo, h) * (1.0f - smoothstepf(hi, hi + 10.0f, h));
    }

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const Plan pl = plan(ctx);
        if (pl.pStrength <= 0.0f && pl.gStrength <= 0.0f) {
            out[0] = Value(src);
            return;
        }
        const int w = src->w, h = src->h, r = pl.radius;
        const bool lin = ctx.linear();
        // Display luminance, then its range (max - min) around each pixel: separable max and min.
        std::vector<float> l(size_t(w) * h), hmax(l.size()), hmin(l.size()), range(l.size());
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const float* s = src->pixel(size_t(y) * w + x);
                const float Y = std::clamp(luminance(s[0], s[1], s[2]), 0.0f, 1.0f);
                l[size_t(y) * w + x] = lin ? colormath::linearToSrgb(Y) : Y;
            }
        });
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                float lo = 1e9f, hi = -1e9f;
                for (int i = std::max(0, x - r); i <= std::min(w - 1, x + r); ++i) {
                    const float v = l[size_t(y) * w + i];
                    lo = std::min(lo, v), hi = std::max(hi, v);
                }
                hmin[size_t(y) * w + x] = lo, hmax[size_t(y) * w + x] = hi;
            }
        });
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                float lo = 1e9f, hi = -1e9f;
                for (int j = std::max(0, y - r); j <= std::min(h - 1, y + r); ++j) {
                    lo = std::min(lo, hmin[size_t(j) * w + x]), hi = std::max(hi, hmax[size_t(j) * w + x]);
                }
                range[size_t(y) * w + x] = hi - lo;
            }
        });
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = s[3];
            const float edge = smoothstepf(0.08f, 0.3f, range[size_t(y) * w + x]);
            if (edge <= 0.0f) return;
            float e[3];
            for (int k = 0; k < 3; ++k) e[k] = clamp01(lin ? colormath::linearToSrgb(s[k]) : s[k]);
            float hu, sat, val;
            colormath::rgbToHsv(e[0], e[1], e[2], hu, sat, val);
            const float deg = hu * 360.0f;
            const float amount = edge * smoothstepf(0.05f, 0.25f, sat) *
                                 (pl.pStrength * hueWindow(deg, pl.pLo, pl.pHi) + pl.gStrength * hueWindow(deg, pl.gLo, pl.gHi));
            const float keep = 1.0f - std::min(amount, 1.0f);
            const float Y = luminance(s[0], s[1], s[2]);
            for (int k = 0; k < 3; ++k) d[k] = clampColor(lin, Y + (s[k] - Y) * keep);
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const Plan pl = plan(ctx);
        gpu::PointOp op;
        if (pl.pStrength <= 0.0f && pl.gStrength <= 0.0f) {
            op.body = "    out0 = img0(p);\n";
            gpu::runOver(ctx, *this, op, in, out);
            return;
        }
        int w, h;
        in[0].size(w, h);
        gpu::PointOp lum;
        lum.w = w, lum.h = h;
        lum.full = true;
        lum.body = "    float Y = clamp(luminance(img0(p).rgb), 0.0, 1.0);\n"
                   "    out0 = uLinear ? linearToSrgb(Y) : Y;\n";
        const Value L = gpu::runPass(ctx, lum, {in[0]}, {false})[0];
        op.gather = {1};
        op.params = {float(pl.radius), pl.pStrength, pl.pLo, pl.pHi, pl.gStrength, pl.gLo, pl.gHi};
        op.functions = R"(
float hueWindow(float h, float lo, float hi) { return smoothstep(lo - 10.0, lo, h) * (1.0 - smoothstep(hi, hi + 10.0, h)); }
)";
        op.body = R"(
    vec4 s = img0(p);
    out0 = s;
    int r = int(P[0]);
    float lo = 1e9, hi = -1e9;
    for (int j = -r; j <= r; ++j)
        for (int i = -r; i <= r; ++i) {
            float v = fetchCh1(p + ivec2(i, j));
            lo = min(lo, v), hi = max(hi, v);
        }
    float edge = smoothstep(0.08, 0.3, hi - lo);
    if (edge <= 0.0) return;
    vec3 e = clamp(uLinear ? linearToSrgb(s.rgb) : s.rgb, 0.0, 1.0);
    vec3 hsv = rgbToHsv(e);
    float deg = hsv.x * 360.0;
    float amount = edge * smoothstep(0.05, 0.25, hsv.y) *
                   (P[1] * hueWindow(deg, P[2], P[3]) + P[4] * hueWindow(deg, P[5], P[6]));
    float keep = 1.0 - min(amount, 1.0);
    float Y = luminance(s.rgb);
    out0 = vec4(clampColor(uLinear, Y + (s.rgb - Y) * keep), s.a);
)";
        gpu::runOver(ctx, *this, op, {in[0], L}, out);
    }
};

// ---------------------------------------------------------------- Border

// darktable's framing and a print border: a coloured margin around the image, optionally a thin
// line around the picture and a canvas of a fixed aspect ratio. Sizes are percentages of the
// image's long edge, so the preview matches the export.
class BorderNode : public Node {
public:
    enum { Size, Bottom, Aspect, Color, LineSize, LineColor };
    REFRACTORY_NODE({"xform.border", "Border", "Transform",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Size", 4.0f, 0.0f, 25.0f), ParamDesc::Float("Bottom", 0.0f, 0.0f, 25.0f),
                   ParamDesc::Enum("Aspect", 0, {"Image", "1:1", "4:5", "5:4", "2:3", "3:2", "16:9", "9:16"}),
                   ParamDesc::Color("Color", 1.0f, 1.0f, 1.0f), ParamDesc::Float("Line Size", 0.0f, 0.0f, 2.0f),
                   ParamDesc::Color("Line Color", 0.0f, 0.0f, 0.0f)}})

    struct Layout {
        int W, H, x0, y0, line;  // canvas size, the picture's position, the line's width
    };
    Layout layout(int w, int h) const {
        const float longEdge = float(std::max(w, h));
        const int b = int(std::lround(paramF(Size) / 100.0f * longEdge));
        const int bottom = int(std::lround(paramF(Bottom) / 100.0f * longEdge));
        Layout l{w + 2 * b, h + 2 * b + bottom, b, b, int(std::lround(paramF(LineSize) / 100.0f * longEdge))};
        static const float kRatios[] = {0.0f, 1.0f, 4.0f / 5, 5.0f / 4, 2.0f / 3, 3.0f / 2, 16.0f / 9, 9.0f / 16};
        const int a = std::clamp(paramI(Aspect), 0, 7);
        if (a > 0) {
            const float ratio = kRatios[a];
            // Grow the canvas (never crop the picture) to the ratio, keeping it centred.
            if (float(l.W) / l.H < ratio) {
                const int W = int(std::lround(l.H * ratio));
                l.x0 += (W - l.W) / 2, l.W = W;
            } else {
                const int H = int(std::lround(l.W / ratio));
                l.y0 += (H - l.H) / 2, l.H = H;
            }
        }
        l.W = std::min(l.W, 65536), l.H = std::min(l.H, 65536);
        return l;
    }
    void roiOutputSize(int inW, int inH, int& w, int& h) const override {
        const Layout l = layout(inW, inH);
        w = l.W, h = l.H;
    }

    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int w = src->w, h = src->h;
        const Layout l = layout(w, h);
        float bg[3], lc[3];
        paramC(Color, bg);
        paramC(LineColor, lc);
        auto img = std::make_shared<Image>(l.W, l.H);
        parallelFor(l.H, [&](int y) {
            for (int x = 0; x < l.W; ++x) {
                float* d = img->pixel(size_t(y) * l.W + x);
                const int ix = x - l.x0, iy = y - l.y0;
                if (ix >= 0 && iy >= 0 && ix < w && iy < h) {
                    // The picture, over the border colour where it is transparent.
                    const float* s = src->pixel(size_t(iy) * w + ix);
                    for (int k = 0; k < 3; ++k) d[k] = s[k] * s[3] + bg[k] * (1.0f - s[3]);
                    d[3] = 1.0f;
                    continue;
                }
                const bool onLine = l.line > 0 && ix >= -l.line && iy >= -l.line && ix < w + l.line && iy < h + l.line;
                for (int k = 0; k < 3; ++k) d[k] = onLine ? lc[k] : bg[k];
                d[3] = 1.0f;
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

}  // namespace

void registerEffectNodes(NodeRegistry& r) {
    r.add<VignetteNode>();
    r.add<DefringeNode>();
    r.add<BorderNode>();
}
