// Restoring detail: Capture Sharpening (RawTherapee's, Richardson-Lucy deconvolution of the
// lens and sensor blur) and darktable's Diffuse or Sharpen (multiscale detail added or removed
// over a few iterations, slowed down across edges). Both are CPU only: each is dozens of
// dependent passes over the image.
#include <algorithm>
#include <cmath>

#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "graph/NodeRegistry.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

float smoothstepf(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / std::max(e1 - e0, 1e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// A normalized Gaussian kernel of radius ceil(3 sigma), as taps from -r to r.
std::vector<float> gaussianKernel(float sigma) {
    const int r = std::max(1, int(std::ceil(3.0f * sigma)));
    std::vector<float> k(size_t(2 * r + 1));
    float sum = 0;
    for (int i = -r; i <= r; ++i) sum += k[size_t(i + r)] = std::exp(-0.5f * float(i * i) / (sigma * sigma));
    for (float& v : k) v /= sum;
    return k;
}

// Separable convolution of a plane with clamped edges.
void convolve(const std::vector<float>& src, std::vector<float>& dst, std::vector<float>& tmp, int w, int h,
              const std::vector<float>& k) {
    const int r = int(k.size() / 2);
    parallelFor(h, [&](int y) {
        const float* row = &src[size_t(y) * w];
        float* o = &tmp[size_t(y) * w];
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int i = -r; i <= r; ++i) s += k[size_t(i + r)] * row[std::clamp(x + i, 0, w - 1)];
            o[x] = s;
        }
    });
    parallelFor(h, [&](int y) {
        float* o = &dst[size_t(y) * w];
        for (int x = 0; x < w; ++x) o[x] = 0;
        for (int i = -r; i <= r; ++i) {
            const float* row = &tmp[size_t(std::clamp(y + i, 0, h - 1)) * w];
            const float kk = k[size_t(i + r)];
            for (int x = 0; x < w; ++x) o[x] += kk * row[x];
        }
    });
}

// ---------------------------------------------------------------- Capture Sharpening

// RawTherapee's Capture Sharpening: the blur the lens, the anti-aliasing filter and demosaicing
// left is close to a small Gaussian, and Richardson-Lucy deconvolution undoes it on luminance.
// Flat areas (below the contrast threshold) are left alone, so noise isn't sharpened.
class CaptureSharpenNode : public Node {
public:
    enum { Radius = 0, Iterations, Threshold, Amount };
    NODELAB_NODE({"filter.capture_sharpen", "Capture Sharpening", "Filter",
                  {{"Image", PinType::Image}, {"Amount", PinType::Channel, Amount}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Radius", 0.75f, 0.4f, 2.0f), ParamDesc::Int("Iterations", 20, 1, 100),
                   ParamDesc::Float("Contrast Threshold", 10.0f, 0.0f, 100.0f), ParamDesc::Float("Amount", 1.0f, 0.0f, 1.0f)}})

    int roiPadding(const EvalContext& ctx) const override {
        const float sigma = paramF(Radius) * ctx.scale;
        if (sigma < kMinSigma) return 0;
        return 2 * int(std::ceil(3.0f * sigma)) * std::clamp(paramI(Iterations), 1, 100) + 2;
    }

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const bool lin = ctx.linear();
        const float sigma = paramF(Radius) * ctx.scale;
        // At a small preview the blur is below a pixel already: nothing to undo.
        if (sigma < kMinSigma) {
            out[0] = Value(src);
            return;
        }
        const int w = src->w, h = src->h, iters = std::clamp(paramI(Iterations), 1, 100);
        const size_t n = size_t(w) * h;
        std::vector<float> Y(n), est(n), blurred(n), ratio(n), tmp(n);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const float* p = src->pixel(size_t(y) * w + x);
                float v = luminance(p[0], p[1], p[2]);
                if (!lin) v = colormath::srgbToLinear(clamp01(v));
                Y[size_t(y) * w + x] = std::isfinite(v) ? std::max(v, 0.0f) : 0.0f;
            }
        });
        const std::vector<float> k = gaussianKernel(sigma);
        constexpr float kEps = 1e-6f;
        est = Y;
        for (int it = 0; it < iters; ++it) {
            convolve(est, blurred, tmp, w, h, k);
            for (size_t i = 0; i < n; ++i) ratio[i] = (Y[i] + kEps) / (blurred[i] + kEps);
            convolve(ratio, blurred, tmp, w, h, k);  // the PSF is symmetric: its mirror is itself
            for (size_t i = 0; i < n; ++i) est[i] *= blurred[i];
        }
        // The contrast mask, on perceptual luminance so the threshold works alike in shadows and
        // highlights.
        const float thr = paramF(Threshold) / 100.0f * 0.2f;
        ChannelPtr amt = channelOr(in[1], paramF(Amount));
        ChannelSampler sa = paramSampler(*this, 1, amt, w, h);
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                float m = 1.0f;
                if (thr > 0.0f) {
                    const auto P = [&](int xx, int yy) {
                        return colormath::linearToSrgb(Y[size_t(std::clamp(yy, 0, h - 1)) * w + std::clamp(xx, 0, w - 1)]);
                    };
                    const float gx = P(x + 1, y) - P(x - 1, y), gy = P(x, y + 1) - P(x, y - 1);
                    m = smoothstepf(thr, 2.0f * thr, 0.5f * std::sqrt(gx * gx + gy * gy));
                }
                // Limited to half or twice the original, as deconvolution rings at hard edges.
                const float r = std::clamp((est[i] + kEps) / (Y[i] + kEps), 0.5f, 2.0f);
                const float g = 1.0f + (r - 1.0f) * m * clamp01(sa(x, y));
                const float* s = src->pixel(i);
                float* d = img->pixel(i);
                for (int c = 0; c < 3; ++c)
                    d[c] = lin ? clampColor(true, s[c] * g)
                               : clamp01(colormath::linearToSrgb(colormath::srgbToLinear(clamp01(s[c])) * g));
                d[3] = s[3];
            }
        });
        out[0] = Value(ImagePtr(img));
    }

private:
    static constexpr float kMinSigma = 0.25f;
};

// ---------------------------------------------------------------- Diffuse or Sharpen

// darktable's Diffuse or Sharpen, simplified: each iteration splits the image into detail
// scales (an a-trous wavelet, as Denoise does), and adds the scales around Radius back (Amount
// above zero: sharpening, deblurring, local contrast) or takes them away (below zero: diffusion,
// a blur that can keep edges). Edge Sensitivity slows it down where the image changes sharply,
// which avoids halos when sharpening and keeps edges when diffusing; details smaller than the
// Noise Threshold aren't sharpened.
class DiffuseNode : public Node {
public:
    enum { Amount = 0, Radius, Span, Iterations, EdgeSensitivity, NoiseThreshold };
    NODELAB_NODE({"filter.diffuse", "Diffuse or Sharpen", "Filter",
                  {{"Image", PinType::Image}, {"Amount", PinType::Channel, Amount}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Amount", 25.0f, -100.0f, 100.0f), ParamDesc::Float("Radius", 4.0f, 0.5f, 128.0f),
                   ParamDesc::Float("Radius Span", 1.0f, 0.25f, 4.0f), ParamDesc::Int("Iterations", 4, 1, 32),
                   ParamDesc::Float("Edge Sensitivity", 50.0f, 0.0f, 100.0f), ParamDesc::Float("Noise Threshold", 5.0f, 0.0f, 100.0f)}})

    int roiPadding(const EvalContext& ctx) const override {
        const Scales s = scales(ctx.scale);
        // Each level reads 2 holes out; the edge test a pixel more.
        long reach = 0;
        for (int j = 0; j < s.levels; ++j) reach += 2L << j;
        reach = (reach + 1) * std::clamp(paramI(Iterations), 1, 32);
        return reach > 4096 ? kRoiWhole : int(reach);
    }

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const bool lin = ctx.linear();
        const int w = src->w, h = src->h, iters = std::clamp(paramI(Iterations), 1, 32);
        const size_t n = size_t(w) * h;
        const Scales sc = scales(ctx.scale);
        // Detail is measured on perceptual values (display-encoded), as darktable works on a
        // log-like scale: thresholds mean the same in the shadows and the highlights.
        std::array<std::vector<float>, 3> cur;
        for (auto& c : cur) c.resize(n);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const float* p = src->pixel(size_t(y) * w + x);
                for (int c = 0; c < 3; ++c) {
                    float v = std::isfinite(p[c]) ? p[c] : 0.0f;
                    cur[size_t(c)][size_t(y) * w + x] = lin ? colormath::linearToSrgb(std::max(v, 0.0f)) : v;
                }
            }
        });
        ChannelPtr amt = channelOr(in[1], paramF(Amount));
        ChannelSampler sa = paramSampler(*this, 1, amt, w, h);
        std::vector<float> amount(n);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) amount[size_t(y) * w + x] = sa(x, y) / 100.0f;
        });
        // Edge Sensitivity 100 halves the update where the luminance changes by 0.05 per pixel
        // (50: by 0.1).
        const float es = paramF(EdgeSensitivity) / 100.0f, k2 = es > 0.0f ? std::pow(0.05f / es, 2.0f) : 0.0f;
        const float thr = paramF(NoiseThreshold) / 100.0f * 0.02f;
        std::vector<float> lum(n), next(n), tmp(n), update(n);
        std::array<std::vector<float>, 3> upd;
        for (auto& u : upd) u.resize(n);
        for (int it = 0; it < iters; ++it) {
            for (auto& u : upd) std::fill(u.begin(), u.end(), 0.0f);
            for (int c = 0; c < 3; ++c) {
                std::vector<float> level = cur[size_t(c)];
                for (int j = 0; j < sc.levels; ++j) {
                    if (sc.weight[size_t(j)] <= 0.0f) {
                        if (j + 1 < sc.levels) atrous(level, next, tmp, w, h, 1 << j), level.swap(next);
                        continue;
                    }
                    atrous(level, next, tmp, w, h, 1 << j);
                    const float wj = sc.weight[size_t(j)];
                    std::vector<float>& u = upd[size_t(c)];
                    parallelFor(h, [&](int y) {
                        for (int x = 0; x < w; ++x) {
                            const size_t i = size_t(y) * w + x;
                            const float d = level[i] - next[i];
                            // Sharpening leaves noise-sized detail alone; diffusing takes it all.
                            const float keep = amount[i] > 0.0f ? smoothstepf(thr, 2.0f * thr, std::fabs(d)) : 1.0f;
                            u[i] += wj * d * keep;
                        }
                    });
                    level.swap(next);
                }
            }
            // The update's speed: slower across edges (Perona-Malik's 1 / (1 + (g / k)^2)).
            parallelFor(h, [&](int y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = size_t(y) * w + x;
                    float g = 1.0f;
                    if (k2 > 0.0f) {
                        const auto L = [&](int xx, int yy) {
                            const size_t q = size_t(std::clamp(yy, 0, h - 1)) * w + std::clamp(xx, 0, w - 1);
                            return luminance(cur[0][q], cur[1][q], cur[2][q]);
                        };
                        const float gx = 0.5f * (L(x + 1, y) - L(x - 1, y)), gy = 0.5f * (L(x, y + 1) - L(x, y - 1));
                        g = 1.0f / (1.0f + (gx * gx + gy * gy) / k2);
                    }
                    // Diffusing removes at most all of a level's detail per iteration (more would
                    // invert it).
                    update[i] = std::max(g * amount[i] / float(iters) * 2.0f, -1.0f);
                }
            });
            for (int c = 0; c < 3; ++c) {
                std::vector<float>& cc = cur[size_t(c)];
                const std::vector<float>& u = upd[size_t(c)];
                parallelFor(h, [&](int y) {
                    for (int x = 0; x < w; ++x) {
                        const size_t i = size_t(y) * w + x;
                        cc[i] = std::max(cc[i] + update[i] * u[i], 0.0f);
                    }
                });
            }
        }
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                float* d = img->pixel(i);
                for (int c = 0; c < 3; ++c) {
                    const float v = cur[size_t(c)][i];
                    d[c] = lin ? clampColor(true, colormath::srgbToLinear(v)) : clamp01(v);
                }
                d[3] = src->pixel(i)[3];
            }
        });
        out[0] = Value(ImagePtr(img));
    }

private:
    struct Scales {
        int levels = 0;
        std::vector<float> weight;  // per level, a Gaussian in log2 scale around the radius
    };
    Scales scales(float scale) const {
        Scales s;
        const float r = std::max(paramF(Radius) * scale, 0.25f), span = std::clamp(paramF(Span), 0.25f, 4.0f);
        const float centre = std::log2(r);
        s.levels = std::clamp(int(std::ceil(centre + 2.0f * span)) + 1, 1, 10);
        float sum = 0;
        for (int j = 0; j < s.levels; ++j) {
            const float d = (float(j) - centre) / span;
            s.weight.push_back(std::exp(-0.5f * d * d));
            sum += s.weight.back();
        }
        for (float& v : s.weight) v = sum > 0.0f ? v / sum * std::min(1.0f, sum) : 0.0f;
        return s;
    }

    // One level of the a-trous wavelet: the B3 spline (1 4 6 4 1) / 16 with holes `step` apart.
    static void atrous(const std::vector<float>& src, std::vector<float>& dst, std::vector<float>& tmp, int w, int h, int step) {
        static constexpr float kB3[5] = {1.0f / 16, 4.0f / 16, 6.0f / 16, 4.0f / 16, 1.0f / 16};
        parallelFor(h, [&](int y) {
            const float* row = &src[size_t(y) * w];
            float* o = &tmp[size_t(y) * w];
            for (int x = 0; x < w; ++x) {
                float s = 0;
                for (int i = -2; i <= 2; ++i) s += kB3[i + 2] * row[std::clamp(x + i * step, 0, w - 1)];
                o[x] = s;
            }
        });
        parallelFor(h, [&](int y) {
            float* o = &dst[size_t(y) * w];
            for (int x = 0; x < w; ++x) o[x] = 0;
            for (int i = -2; i <= 2; ++i) {
                const float* row = &tmp[size_t(std::clamp(y + i * step, 0, h - 1)) * w];
                for (int x = 0; x < w; ++x) o[x] += kB3[i + 2] * row[x];
            }
        });
    }
};

}  // namespace

void registerSharpeningNodes(NodeRegistry& r) {
    r.add<CaptureSharpenNode>();
    r.add<DiffuseNode>();
}
