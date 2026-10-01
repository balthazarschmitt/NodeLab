// Filter nodes: blurs, convolution filters, morphology, stylization, glare.
// Sizes are in full-resolution pixels; ctx.scale converts them for the preview proxy.
#include <cmath>

#include "gpu/Blur.h"
#include "gpu/PointOp.h"
#include "nodes/ImageOps.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;
using namespace imageops;

namespace {

constexpr float kPi = 3.14159265f;

std::shared_ptr<Image> copyOf(const Image& src) { return std::make_shared<Image>(src); }

// ---------------------------------------------------------------- blurs

class BlurNode : public Node {
public:
    NODELAB_NODE({"filter.blur", "Blur", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Size X", 10.0f, 0.0f, 200.0f).when(2, 0), ParamDesc::Float("Size Y", 10.0f, 0.0f, 200.0f).when(2, 0),
                   ParamDesc::Bool("Relative", false), ParamDesc::Enum("Aspect Correction", 0, {"None", "Y", "X"}).when(2),
                   ParamDesc::Float("Factor X", 1.0f, 0.0f, 100.0f).when(2), ParamDesc::Float("Factor Y", 1.0f, 0.0f, 100.0f).when(2)}})
    // Blur sizes (pixels) for an image of w x h.
    void sizes(const EvalContext& ctx, int w, int h, float& sx, float& sy) const {
        sx = paramF(0) * ctx.scale;
        sy = paramF(1) * ctx.scale;
        if (paramB(2)) {
            // Blender's Relative: percent of the image size, so one setting fits any resolution
            // (the working image is already proxy-sized, so no ctx.scale). Aspect Correction Y
            // measures Y against the width too (round blur on non-square images), X the reverse.
            const int ac = paramI(3);
            sx = paramF(4) * 0.01f * float(ac == 2 ? h : w);
            sy = paramF(5) * 0.01f * float(ac == 1 ? w : h);
        }
    }
    int roiPadding(const EvalContext& ctx) const override {
        const PixelFrame fr = frameOf(ctx, ctx.defaultW, ctx.defaultH);
        float sx, sy;
        sizes(ctx, fr.fullW, fr.fullH, sx, sy);
        return std::max(blurReach(sx * 0.5f), blurReach(sy * 0.5f)) + 1;
    }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        // A channel (e.g. a mask) is blurred as one plane and passed on as a channel: converting
        // it to a grey RGBA image first would blur four identical planes. Consumers convert it
        // back to an image exactly as they would have (grey, alpha 1), so results don't change.
        const auto* chp = std::get_if<ChannelPtr>(&in[0].v);
        const Channel* srcCh = chp && *chp && !(*chp)->constant ? chp->get() : nullptr;
        if (chp && *chp && (*chp)->constant) {
            out[0] = in[0];  // flat: nothing to blur
            return;
        }
        ImagePtr src = srcCh ? nullptr : toImage(in[0], 0, 0);
        if (!src && !srcCh) return;
        const int w = srcCh ? srcCh->w : src->w, h = srcCh ? srcCh->h : src->h;
        const PixelFrame fr = frameOf(ctx, w, h);
        float sx, sy;
        sizes(ctx, fr.fullW, fr.fullH, sx, sy);
        if (srcCh) {
            auto c = std::make_shared<Channel>(*srcCh);
            blurChannel(c->data, w, h, sx * 0.5f, sy * 0.5f);
            out[0] = Value(ChannelPtr(c));
            return;
        }
        auto img = copyOf(*src);
        blurImage(*img, sx * 0.5f, sy * 0.5f);
        out[0] = Value(ImagePtr(img));
    }

    // Images and sized channels (numbers and flat channels have nothing to blur).
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const Value v = in[0].onGpu() ? in[0] : gpu::toGpu(in[0], ctx.gpuHalf);
        int w, h;
        v.size(w, h);
        const PixelFrame fr = frameOf(ctx, w, h);
        float sx, sy;
        sizes(ctx, fr.fullW, fr.fullH, sx, sy);
        if (auto c = std::get_if<GpuChannelPtr>(&v.v)) {
            auto r = std::make_shared<GpuChannel>();
            r->tex = gpu::boxBlur((*c)->tex, sx * 0.5f, sy * 0.5f), r->w = w, r->h = h;
            out[0] = Value(GpuChannelPtr(r));
        } else if (auto i = std::get_if<GpuImagePtr>(&v.v)) {
            auto r = std::make_shared<GpuImage>();
            r->tex = gpu::boxBlur((*i)->tex, sx * 0.5f, sy * 0.5f), r->w = w, r->h = h;
            out[0] = Value(GpuImagePtr(r));
        }
    }
};

class DirectionalBlurNode : public Node {
public:
    NODELAB_NODE({"filter.directional_blur", "Directional Blur", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Distance", 30.0f, 0.0f, 400.0f), ParamDesc::Float("Angle", 0.0f, -180.0f, 180.0f),
                   ParamDesc::Float("Spin", 0.0f, -90.0f, 90.0f), ParamDesc::Float("Zoom", 0.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Center X", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Center Y", 0.5f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float dist = paramF(0) * ctx.scale, ang = paramF(1) * kPi / 180.0f;
        const float spin = paramF(2) * kPi / 180.0f, zoom = paramF(3);
        const float cx = paramF(4) * src->w, cy = paramF(5) * src->h;
        const int steps = std::clamp(int(std::max({dist, std::fabs(spin) * src->w * 0.5f, zoom * src->w * 0.5f})), 1, 96);
        const float dx = std::cos(ang) * dist, dy = std::sin(ang) * dist;
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float*, float* d) {
            float acc[4] = {0, 0, 0, 0};
            const float px = x + 0.5f - cx, py = y + 0.5f - cy;
            for (int s = 0; s < steps; ++s) {
                float t = steps > 1 ? float(s) / (steps - 1) : 0.0f;
                float a = spin * t, sc = 1.0f - zoom * t;
                float rx = (px * std::cos(a) - py * std::sin(a)) * sc + cx + dx * t;
                float ry = (px * std::sin(a) + py * std::cos(a)) * sc + cy + dy * t;
                float c[4];
                sampleBilinear(*src, rx, ry, c);
                for (int k = 0; k < 4; ++k) acc[k] += c[k];
            }
            for (int k = 0; k < 4; ++k) d[k] = acc[k] / steps;
        })));
    }
};

class BilateralBlurNode : public Node {
public:
    NODELAB_NODE({"filter.bilateral_blur", "Bilateral Blur", "Filter",
                  {{"Image", PinType::Image}, {"Determinator", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Radius", 8.0f, 0.0f, 60.0f), ParamDesc::Float("Color Sigma", 0.1f, 0.001f, 1.0f)}})
    int roiPadding(const EvalContext& ctx) const override { return int(std::ceil(paramF(0) * ctx.scale)) + 1; }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ImagePtr det = toImage(in[1], src->w, src->h);
        if (!det || det->w != src->w || det->h != src->h) det = src;  // edges come from the image itself
        const float r = paramF(0) * ctx.scale;
        const float cs = paramF(1);
        const int ri = int(std::ceil(r));
        const int step = std::max(1, ri / 7);  // cap the kernel at ~15x15 taps
        const float inv2s2 = 1.0f / (2.0f * std::max(r * 0.5f, 0.5f) * std::max(r * 0.5f, 0.5f));
        const float inv2c2 = 1.0f / (2.0f * cs * cs);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            const float* c0 = det->pixel(size_t(y) * src->w + x);
            float acc[4] = {0, 0, 0, 0}, wsum = 0;
            for (int j = -ri; j <= ri; j += step)
                for (int i = -ri; i <= ri; i += step) {
                    int xx = std::clamp(x + i, 0, src->w - 1), yy = std::clamp(y + j, 0, src->h - 1);
                    size_t idx = size_t(yy) * src->w + xx;
                    const float* c = det->pixel(idx);
                    float dc = (c[0] - c0[0]) * (c[0] - c0[0]) + (c[1] - c0[1]) * (c[1] - c0[1]) + (c[2] - c0[2]) * (c[2] - c0[2]);
                    float w = std::exp(-float(i * i + j * j) * inv2s2 - dc * inv2c2);
                    const float* p = src->pixel(idx);
                    for (int k = 0; k < 4; ++k) acc[k] += p[k] * w;
                    wsum += w;
                }
            for (int k = 0; k < 4; ++k) d[k] = wsum > 0 ? acc[k] / wsum : s[k];
        })));
    }
};

// ---------------------------------------------------------------- 3x3 filters

class FilterNode : public Node {
public:
    enum { Soften, BoxSharpen, DiamondSharpen, Laplace, Sobel, Prewitt, Kirsch, Shadow };
    NODELAB_NODE({"filter.filter", "Filter", "Filter",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Enum("Type", Sobel, {"Soften", "Box Sharpen", "Diamond Sharpen", "Laplace", "Sobel", "Prewitt",
                                                   "Kirsch", "Shadow"})}})
    int roiPadding(const EvalContext&) const override { return 1; }  // 3x3 kernels
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int type = paramI(1);
        ChannelPtr fac = channelOr(in[1], 1.0f);
        ChannelSampler sf = paramSampler(*this, 1, fac, src->w, src->h);
        auto at = [&](int x, int y, int k) {
            return src->pixel(size_t(std::clamp(y, 0, src->h - 1)) * src->w + std::clamp(x, 0, src->w - 1))[k];
        };
        // Kernels as 3x3 rows; edge detectors use the magnitude of a kernel and its transpose.
        static const float soften[9] = {1, 2, 1, 2, 4, 2, 1, 2, 1};
        static const float box[9] = {-1, -1, -1, -1, 9, -1, -1, -1, -1};
        static const float diamond[9] = {0, -1, 0, -1, 5, -1, 0, -1, 0};
        static const float laplace[9] = {-1, -1, -1, -1, 8, -1, -1, -1, -1};
        static const float sobel[9] = {-1, 0, 1, -2, 0, 2, -1, 0, 1};
        static const float prewitt[9] = {-1, 0, 1, -1, 0, 1, -1, 0, 1};
        static const float kirsch[9] = {-3, -3, 5, -3, 0, 5, -3, -3, 5};
        static const float shadow[9] = {1, 2, 1, 0, 1, 0, -1, -2, -1};
        auto conv = [&](const float* kr, int x, int y, int k, bool transpose) {
            float s = 0;
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i) s += kr[transpose ? i * 3 + j : j * 3 + i] * at(x + i - 1, y + j - 1, k);
            return s;
        };
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            for (int k = 0; k < 3; ++k) {
                float v;
                switch (type) {
                    case Soften: v = conv(soften, x, y, k, false) / 16.0f; break;
                    case BoxSharpen: v = conv(box, x, y, k, false); break;
                    case DiamondSharpen: v = conv(diamond, x, y, k, false); break;
                    case Laplace: v = conv(laplace, x, y, k, false) / 8.0f + 0.0f; break;
                    case Sobel: v = std::hypot(conv(sobel, x, y, k, false), conv(sobel, x, y, k, true)) * 0.25f; break;
                    case Prewitt: v = std::hypot(conv(prewitt, x, y, k, false), conv(prewitt, x, y, k, true)) / 3.0f; break;
                    case Kirsch: v = std::hypot(conv(kirsch, x, y, k, false), conv(kirsch, x, y, k, true)) / 15.0f; break;
                    default: v = conv(shadow, x, y, k, false) + 0.0f; break;
                }
                float f = sf(x, y);
                d[k] = clampColor(ctx.linear(), s[k] + (v - s[k]) * f);
            }
            d[3] = s[3];
        })));
    }
};

// ---------------------------------------------------------------- morphology

class DilateErodeNode : public Node {
public:
    NODELAB_NODE({"filter.dilate_erode", "Dilate / Erode", "Filter",
                  {{"Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Enum("Mode", 0, {"Distance", "Feather"}), ParamDesc::Float("Distance", 5.0f, -100.0f, 100.0f)}})
    // Only distances up to Distance matter: anything farther gives the same result.
    int roiPadding(const EvalContext& ctx) const override { return int(std::ceil(std::fabs(paramF(1) * ctx.scale))) + 2; }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ChannelPtr m = toChannel(in[0]);
        if (!m || m->constant) {
            if (m) out[0] = Value(m);
            return;
        }
        const int w = m->w, h = m->h;
        const float dist = paramF(1) * ctx.scale;
        const bool feather = paramI(0) == 1;
        std::vector<uint8_t> inside(size_t(w) * h), outside(size_t(w) * h);
        for (size_t i = 0; i < inside.size(); ++i) {
            inside[i] = m->data[i] >= 0.5f;
            outside[i] = !inside[i];
        }
        // Dilate: grow by distance to the mask. Erode: shrink by distance to the background.
        std::vector<float> dt = distanceTransform(dist >= 0 ? inside : outside, w, h);
        const float ad = std::max(std::fabs(dist), 1e-3f);
        auto res = std::make_shared<Channel>(Channel::makeSized(w, h));
        for (size_t i = 0; i < dt.size(); ++i) {
            float v;
            if (dist >= 0) v = feather ? std::max(m->data[i], clamp01(1.0f - dt[i] / ad)) : (dt[i] <= ad ? 1.0f : 0.0f);
            else v = feather ? std::min(m->data[i], clamp01(dt[i] / ad)) : (dt[i] > ad ? 1.0f : 0.0f);
            res->data[i] = v;
        }
        out[0] = Value(ChannelPtr(res));
    }
};

// ---------------------------------------------------------------- stylize

class KuwaharaNode : public Node {
public:
    NODELAB_NODE({"filter.kuwahara", "Kuwahara", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Size", 6.0f, 1.0f, 50.0f)}})
    int roiPadding(const EvalContext& ctx) const override {
        return std::max(1, int(std::round(paramF(0) * ctx.scale))) + 1;
    }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int w = src->w, h = src->h;
        const int r = std::max(1, int(std::round(paramF(0) * ctx.scale)));
        // Summed-area tables of r, g, b, luminance and luminance^2 for O(1) region stats.
        const size_t W = size_t(w) + 1;
        std::vector<double> sat[5];
        for (auto& t : sat) t.assign(W * (h + 1), 0.0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float* p = src->pixel(size_t(y) * w + x);
                double l = luminance(p[0], p[1], p[2]);
                double v[5] = {p[0], p[1], p[2], l, l * l};
                for (int k = 0; k < 5; ++k)
                    sat[k][(y + 1) * W + x + 1] = v[k] + sat[k][y * W + x + 1] + sat[k][(y + 1) * W + x] - sat[k][y * W + x];
            }
        auto region = [&](int k, int x0, int y0, int x1, int y1) {  // inclusive-exclusive, clamped
            x0 = std::clamp(x0, 0, w), x1 = std::clamp(x1, 0, w), y0 = std::clamp(y0, 0, h), y1 = std::clamp(y1, 0, h);
            return sat[k][y1 * W + x1] - sat[k][y0 * W + x1] - sat[k][y1 * W + x0] + sat[k][y0 * W + x0];
        };
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            const int qx[4][2] = {{x - r, x + 1}, {x, x + r + 1}, {x - r, x + 1}, {x, x + r + 1}};
            const int qy[4][2] = {{y - r, y + 1}, {y - r, y + 1}, {y, y + r + 1}, {y, y + r + 1}};
            double best = 1e30;
            for (int q = 0; q < 4; ++q) {
                int x0 = std::clamp(qx[q][0], 0, w), x1 = std::clamp(qx[q][1], 0, w);
                int y0 = std::clamp(qy[q][0], 0, h), y1 = std::clamp(qy[q][1], 0, h);
                double n = double(x1 - x0) * (y1 - y0);
                if (n <= 0) continue;
                double mean = region(3, x0, y0, x1, y1) / n;
                double var = region(4, x0, y0, x1, y1) / n - mean * mean;
                if (var < best) {
                    best = var;
                    for (int k = 0; k < 3; ++k) d[k] = float(region(k, x0, y0, x1, y1) / n);
                }
            }
            d[3] = s[3];
        })));
    }
};

class PixelateNode : public Node {
public:
    NODELAB_NODE({"filter.pixelate", "Pixelate", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Size", 16.0f, 1.0f, 200.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int bs = std::max(1, int(std::round(paramF(0) * ctx.scale)));
        const int w = src->w, h = src->h;
        auto img = std::make_shared<Image>(w, h);
        const int bw = (w + bs - 1) / bs, bh = (h + bs - 1) / bs;
        parallelFor(bh, [&](int by) {
            for (int bx = 0; bx < bw; ++bx) {
                int x0 = bx * bs, y0 = by * bs, x1 = std::min(w, x0 + bs), y1 = std::min(h, y0 + bs);
                float acc[4] = {0, 0, 0, 0};
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                        for (int k = 0; k < 4; ++k) acc[k] += src->pixel(size_t(y) * w + x)[k];
                float inv = 1.0f / float((x1 - x0) * (y1 - y0));
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                        for (int k = 0; k < 4; ++k) img->pixel(size_t(y) * w + x)[k] = acc[k] * inv;
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

class PosterizeNode : public Node {
public:
    NODELAB_NODE({"filter.posterize", "Posterize", "Filter",
                  {{"Image", PinType::Image}, {"Steps", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Steps", 6.0f, 2.0f, 64.0f)}})
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr st = channelOr(in[1], 6.0f);
        ChannelSampler ss = paramSampler(*this, 1, st, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float n = std::max(2.0f, std::round(ss(x, y))) - 1.0f;
            for (int k = 0; k < 3; ++k) d[k] = std::round(clamp01(s[k]) * n) / n;
            d[3] = s[3];
        })));
    }
};

// ---------------------------------------------------------------- light effects

class GlareNode : public Node {
public:
    enum { FogGlow, Streaks, SimpleStar };
    NODELAB_NODE({"filter.glare", "Glare", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}, {"Glare", PinType::Image}},
                  {ParamDesc::Enum("Type", FogGlow, {"Fog Glow", "Streaks", "Simple Star"}),
                   ParamDesc::Float("Threshold", 0.75f, 0.0f, 1.0f), ParamDesc::Float("Size", 60.0f, 1.0f, 500.0f),
                   ParamDesc::Float("Strength", 1.0f, 0.0f, 4.0f), ParamDesc::Float("Streaks", 4.0f, 2.0f, 16.0f),
                   ParamDesc::Float("Angle", 30.0f, 0.0f, 180.0f), ParamDesc::Float("Fade", 0.9f, 0.5f, 0.99f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int type = paramI(0);
        const float thr = paramF(1), size = paramF(2) * ctx.scale, strength = paramF(3);
        const int w = src->w, h = src->h;

        // Highlights above the threshold, rescaled so the brightest parts reach 1.
        auto hi = mapImage(*src, [&](int, int, const float* s, float* d) {
            for (int k = 0; k < 3; ++k) d[k] = std::max(s[k] - thr, 0.0f) / std::max(1.0f - thr, 1e-3f);
            d[3] = 1.0f;
        });

        std::shared_ptr<Image> glare;
        if (type == FogGlow) {
            // Two blur widths summed give a tight core plus a wide halo.
            auto wide = std::make_shared<Image>(*hi);
            blurImage(*wide, size * 0.5f, size * 0.5f);
            blurImage(*hi, size * 0.12f, size * 0.12f);
            glare = mapImage(*wide, [&](int x, int y, const float* a, float* d) {
                const float* b = hi->pixel(size_t(y) * w + x);
                for (int k = 0; k < 3; ++k) d[k] = a[k] * 0.7f + b[k] * 0.6f;
                d[3] = 1.0f;
            });
        } else {
            // Streaks: for each direction, repeatedly add a shifted, faded copy with doubling offsets
            // (exponential tail in log2(length) passes).
            const int n = type == SimpleStar ? 4 : std::max(2, int(paramF(4)));
            const float angle0 = paramF(5) * kPi / 180.0f, fade = paramF(6);
            glare = std::make_shared<Image>(w, h);
            for (int s = 0; s < n; ++s) {
                float a = angle0 + (type == SimpleStar ? s * kPi / 4.0f : s * 2.0f * kPi / n);
                float dx = std::cos(a), dy = std::sin(a);
                auto cur = std::make_shared<Image>(*hi);
                for (float step = 1.0f; step < size; step *= 2.0f) {
                    float wgt = std::pow(fade, step);
                    auto next = mapImage(*cur, [&](int x, int y, const float* c, float* d) {
                        float o[4], o2[4];
                        sampleBilinear(*cur, x + 0.5f - dx * step, y + 0.5f - dy * step, o, true);
                        sampleBilinear(*cur, x + 0.5f + dx * step, y + 0.5f + dy * step, o2, true);
                        for (int k = 0; k < 3; ++k) d[k] = c[k] + (o[k] + o2[k]) * wgt;
                        d[3] = 1.0f;
                    });
                    cur = next;
                }
                for (size_t i = 0; i < glare->pixelCount(); ++i)
                    for (int k = 0; k < 3; ++k) glare->pixel(i)[k] += cur->pixel(i)[k] / (n * std::max(1.0f, std::log2(size)));
            }
        }
        auto result = mapImage(*src, [&](int x, int y, const float* s, float* d) {
            const float* gl = glare->pixel(size_t(y) * w + x);
            for (int k = 0; k < 3; ++k) d[k] = clampColor(ctx.linear(), s[k] + gl[k] * strength);
            d[3] = s[3];
        });
        for (size_t i = 0; i < glare->pixelCount(); ++i) {
            for (int k = 0; k < 3; ++k) glare->pixel(i)[k] = clampColor(ctx.linear(), glare->pixel(i)[k] * strength);
            glare->pixel(i)[3] = 1.0f;
        }
        out[0] = Value(ImagePtr(result));
        out[1] = Value(ImagePtr(glare));
    }
};

class SunBeamsNode : public Node {
public:
    NODELAB_NODE({"filter.sun_beams", "Sun Beams", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Source X", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Source Y", 0.3f, 0.0f, 1.0f),
                   ParamDesc::Float("Length", 0.3f, 0.0f, 1.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float sx = paramF(0) * src->w, sy = paramF(1) * src->h, len = paramF(2);
        const int samples = 48;
        // Each pixel averages the image along the line toward the source: bright areas smear outward.
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float acc[3] = {0, 0, 0}, wsum = 0;
            float px = x + 0.5f, py = y + 0.5f;
            for (int i = 0; i < samples; ++i) {
                float t = len * float(i) / samples;
                float c[4];
                sampleBilinear(*src, px + (sx - px) * t, py + (sy - py) * t, c);
                float wgt = 1.0f - float(i) / samples;
                for (int k = 0; k < 3; ++k) acc[k] += c[k] * wgt;
                wsum += wgt;
            }
            for (int k = 0; k < 3; ++k) d[k] = acc[k] / wsum;
            d[3] = s[3];
        })));
    }
};

}  // namespace

void registerFilterNodes(NodeRegistry& r) {
    r.add<BlurNode>();
    r.add<DirectionalBlurNode>();
    r.add<BilateralBlurNode>();
    r.add<FilterNode>();
    r.add<DilateErodeNode>();
    r.add<KuwaharaNode>();
    r.add<PixelateNode>();
    r.add<PosterizeNode>();
    r.add<GlareNode>();
    r.add<SunBeamsNode>();
}
