// Photo "develop" nodes modelled on Lightroom's panels: Basic, Color Mixer (HSL) and Color
// Grading. Sliders use Lightroom's -100..100 scale so numbers carry over.
//
// Scene-linear projects use darktable-style maths on linear light: CAT16 white balance, a tone
// equalizer for Highlights/Shadows (gains per exposure band, driven by an edge-aware luminance
// mask), contrast and local contrast as ratios in log space, and colour work in Oklch. Values are
// unbounded until the view transform, so Highlights can recover what Exposure pushed past white.
// Legacy projects keep the original display-referred maths (and its 0..1 clamp) unchanged.
#include <array>
#include <cmath>

#include "core/ColorMath.h"
#include "core/ColorScience.h"
#include "nodes/ImageOps.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;
using namespace colormath;

namespace {

float smooth(float e0, float e1, float x) {
    float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float luma(const float* p) { return luminance(p[0], p[1], p[2]); }

// Edge-preserving smoothing (He et al.'s guided filter, guided by the image itself): flat areas
// are blurred, while edges with more contrast than about sqrt(eps) are kept. Clarity uses it as
// its base so strong edges (a tree against the sky) don't get halos.
std::vector<float> guidedSmooth(const std::vector<float>& I, int w, int h, float sigma, float eps) {
    const size_t n = I.size();
    std::vector<float> mean = I, sq(n);
    for (size_t i = 0; i < n; ++i) sq[i] = I[i] * I[i];
    imageops::blurChannel(mean, w, h, sigma, sigma);
    imageops::blurChannel(sq, w, h, sigma, sigma);
    std::vector<float> a(n), b(n);
    for (size_t i = 0; i < n; ++i) {
        const float var = std::max(sq[i] - mean[i] * mean[i], 0.0f);
        a[i] = var / (var + eps);
        b[i] = mean[i] - a[i] * mean[i];
    }
    imageops::blurChannel(a, w, h, sigma, sigma);
    imageops::blurChannel(b, w, h, sigma, sigma);
    for (size_t i = 0; i < n; ++i) mean[i] = a[i] * I[i] + b[i];
    return mean;
}

// He & Sun's fast guided filter: the same result as guidedSmooth, but the linear coefficients a, b
// are solved at 1/s resolution and upsampled before applying them to the full-resolution guide, so
// edges stay sharp while the cost drops by about s^2. s is chosen so the filter radius stays at
// least ~4 low-res pixels (the coefficients are smooth at that scale).
std::vector<float> fastGuidedSmooth(const std::vector<float>& I, int w, int h, float sigma, float eps) {
    const int s = std::clamp(int(sigma / 4.0f), 1, 8);
    if (s == 1) return guidedSmooth(I, w, h, sigma, eps);
    const int lw = (w + s - 1) / s, lh = (h + s - 1) / s;
    const size_t ln = size_t(lw) * lh;
    std::vector<float> mean(ln), sq(ln);
    parallelFor(lh, [&](int ly) {
        for (int lx = 0; lx < lw; ++lx) {
            float sum = 0, sum2 = 0;
            int cnt = 0;
            for (int y = ly * s; y < std::min((ly + 1) * s, h); ++y)
                for (int x = lx * s; x < std::min((lx + 1) * s, w); ++x) {
                    const float v = I[size_t(y) * w + x];
                    sum += v, sum2 += v * v, ++cnt;
                }
            mean[size_t(ly) * lw + lx] = sum / cnt;
            sq[size_t(ly) * lw + lx] = sum2 / cnt;
        }
    });
    const float ls = sigma / s;
    imageops::blurChannel(mean, lw, lh, ls, ls);
    imageops::blurChannel(sq, lw, lh, ls, ls);
    std::vector<float> a(ln), b(ln);
    for (size_t i = 0; i < ln; ++i) {
        const float var = std::max(sq[i] - mean[i] * mean[i], 0.0f);
        a[i] = var / (var + eps);
        b[i] = mean[i] - a[i] * mean[i];
    }
    imageops::blurChannel(a, lw, lh, ls, ls);
    imageops::blurChannel(b, lw, lh, ls, ls);
    std::vector<float> q(I.size());
    const float inv = 1.0f / s;
    parallelFor(h, [&](int y) {
        const float ly = (y + 0.5f) * inv;
        for (int x = 0; x < w; ++x) {
            const float lx = (x + 0.5f) * inv;
            const size_t i = size_t(y) * w + x;
            q[i] = imageops::sampleBilinear(a, lw, lh, lx, ly) * I[i] + imageops::sampleBilinear(b, lw, lh, lx, ly);
        }
    });
    return q;
}

// ---------------------------------------------------------------- scene-linear helpers

constexpr float kMidGreyEv = -2.4739312f;  // log2(0.18)

// Exposure of a luminance in stops relative to 1.0 (white in the Standard view).
float evOf(float y) { return std::log2(std::max(y, 1.0f / 65536.0f)); }

std::vector<float> logLuminance(const Image& img) {
    std::vector<float> ev(size_t(img.w) * img.h);
    parallelFor(img.h, [&](int y) {
        for (int x = 0; x < img.w; ++x) ev[size_t(y) * img.w + x] = evOf(luma(img.pixel(size_t(y) * img.w + x)));
    });
    return ev;
}

// Colours come out of the Oklch edits slightly outside the RGB gamut; linear projects keep values
// above 1, so only negatives need fixing.
void finishLinear(Image& img) {
    parallelFor(img.h, [&](int y) {
        for (int x = 0; x < img.w; ++x) colorsci::compressToGamut(img.pixel(size_t(y) * img.w + x));
    });
}

void toOklch(const float* rgb, float& L, float& C, float& hDeg) {
    float lab[3];
    colorsci::rgbToOklab(rgb, lab);
    L = lab[0];
    C = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
    hDeg = std::atan2(lab[2], lab[1]) * 57.29578f;
    if (hDeg < 0) hDeg += 360.0f;
}

void fromOklch(float L, float C, float hDeg, float* rgb) {
    const float h = hDeg * 0.017453293f;
    const float lab[3] = {L, C * std::cos(h), C * std::sin(h)};
    colorsci::oklabToRgb(lab, rgb);
}

// Blends the adjusted image back over the source by a Factor channel (Lightroom's masks plug in here).
void applyFactor(const Node& node, const Image& src, Image& img, const Value& facIn) {
    ChannelPtr fac = channelOr(facIn, 1.0f);
    if (fac->constant && fac->value >= 1.0f) return;
    ChannelSampler sf = paramSampler(node, 1, fac, src.w, src.h);
    parallelFor(src.h, [&](int y) {
        for (int x = 0; x < src.w; ++x) {
            size_t i = size_t(y) * src.w + x;
            float f = sf(x, y);
            const float* s = src.pixel(i);
            float* d = img.pixel(i);
            for (int k = 0; k < 3; ++k) d[k] = s[k] + (d[k] - s[k]) * f;
        }
    });
}

// ---------------------------------------------------------------- Basic

// Contrast S-curve on 0..1 through (0.5, 0.5): p > 1 steepens the middle, p < 1 flattens it.
float sCurve(float x, float p) {
    float xc = std::clamp(x, 0.0f, 1.0f);
    float y = xc < 0.5f ? 0.5f * std::pow(2.0f * xc, p) : 1.0f - 0.5f * std::pow(2.0f - 2.0f * xc, p);
    return y + (x - xc);  // values outside 0..1 (before Highlights recovers them) pass through
}

struct ToneParams {
    float contrast, highlights, shadows, whites, blacks;  // -1..1
};

// Highlights, Shadows, Whites and Blacks as one curve on luminance, in Lightroom's order.
float toneCurve(float v, const ToneParams& t) {
    if (t.highlights != 0.0f) {
        float w = smooth(0.35f, 1.0f, v);
        // Negative compresses proportionally, so values above 1 come back into range; positive
        // brightens but tapers off before it would clip everything.
        if (t.highlights < 0) v += t.highlights * 0.22f * w * std::max(v, 0.0f);
        else v += t.highlights * 0.2f * w * std::clamp(1.25f - v, 0.0f, 1.0f);
    }
    if (t.shadows != 0.0f) {
        float w = 1.0f - smooth(0.0f, 0.6f, v);
        // Positive lifts dark areas but leaves pure black alone.
        if (t.shadows > 0) v += t.shadows * 0.18f * w * smooth(0.0f, 0.2f, v);
        else v += t.shadows * 0.4f * w * std::max(v, 0.0f);
    }
    if (t.whites != 0.0f) {
        const float wp = 1.0f - 0.2f * t.whites;  // white point: < 1 clips earlier
        v += (v / wp - v) * smooth(0.4f, 1.0f, v);
    }
    if (t.blacks != 0.0f) {
        const float bp = -0.08f * t.blacks;  // black point: > 0 crushes, < 0 lifts
        v += ((v - bp) / (1.0f - bp) - v) * (1.0f - smooth(0.0f, 0.5f, v));
    }
    return v;
}

class BasicNode : public Node {
public:
    NODELAB_NODE({"color.basic", "Basic", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Temperature", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Tint", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Exposure", 0.0f, -5.0f, 5.0f), ParamDesc::Float("Contrast", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Highlights", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Shadows", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Whites", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Blacks", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Texture", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Clarity", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Dehaze", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Vibrance", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Saturation", 0.0f, -100.0f, 100.0f)},
                  false, true})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        if (ctx.linear()) {
            out[0] = Value(evaluateLinear(src, in[1]));
            return;
        }
        const float temp = paramF(1) / 100, tint = paramF(2) / 100, stops = paramF(3);
        const ToneParams tone{paramF(4) / 100, paramF(5) / 100, paramF(6) / 100, paramF(7) / 100, paramF(8) / 100};
        const float texture = paramF(9) / 100, clarity = paramF(10) / 100, dehaze = paramF(11) / 100;
        const float vibrance = paramF(12) / 100, saturation = paramF(13) / 100;
        const int w = src->w, h = src->h;
        const float longEdge = float(std::max(w, h));

        // White balance and exposure in linear light. Temperature warms (more red, less blue),
        // Tint goes toward magenta (less green); the gains are normalised so white keeps its
        // brightness and only Exposure changes it.
        float gain[3] = {std::exp2(0.7f * temp), std::exp2(-0.5f * tint), std::exp2(-0.7f * temp)};
        const float norm = std::exp2(stops) / luminance(gain[0], gain[1], gain[2]);
        for (float& g : gain) g *= norm;
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            for (int k = 0; k < 3; ++k) d[k] = linearToSrgb(srgbToLinear(std::max(s[k], 0.0f)) * gain[k]);
            d[3] = s[3];
        });

        if (dehaze != 0.0f) dehazeImage(*img, dehaze, longEdge, 0.3f);

        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                float* d = img->pixel(size_t(y) * w + x);
                // Contrast is a per-channel curve (it adds saturation, as in Lightroom); the other
                // tone sliders scale RGB by the luminance change, so colours keep their saturation.
                if (tone.contrast != 0.0f) {
                    const float p = 1.0f + tone.contrast * (tone.contrast > 0 ? 0.9f : 0.6f);
                    for (int k = 0; k < 3; ++k) d[k] = sCurve(d[k], p);
                }
                const float l = std::max(luma(d), 0.0f), nl = toneCurve(l, tone);
                if (l > 1e-3f) {
                    const float m = std::min(nl / l, 4.0f);
                    for (int k = 0; k < 3; ++k) d[k] *= m;
                } else {
                    for (int k = 0; k < 3; ++k) d[k] += nl - l;
                }
            }
        });

        // Local contrast: detail = luma minus a blurred luma. Texture uses a small radius (fine
        // detail), Clarity a large one weighted toward midtones. Radii are relative to the image
        // so the preview matches the export.
        std::vector<float> lum, coarse, fine;
        if (clarity != 0.0f || texture != 0.0f) {
            lum.resize(size_t(w) * h);
            parallelFor(h, [&](int y) {
                for (int x = 0; x < w; ++x) lum[size_t(y) * w + x] = std::clamp(luma(img->pixel(size_t(y) * w + x)), 0.0f, 1.5f);
            });
            if (clarity != 0.0f) coarse = guidedSmooth(lum, w, h, std::max(longEdge * 0.012f, 1.0f), 0.004f);
            if (texture != 0.0f) {
                fine = lum;
                const float sg = std::max(longEdge * 0.0025f, 0.7f);
                imageops::blurChannel(fine, w, h, sg, sg);
            }
        }

        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                float* d = img->pixel(i);
                if (!lum.empty()) {
                    const float l = lum[i];
                    float delta = 0.0f;
                    if (!coarse.empty()) {
                        float dt = l - coarse[i];
                        dt /= 1.0f + 3.0f * std::fabs(dt);  // soft limit: no harsh overshoot
                        float mid = std::max(0.0f, 1.0f - (2.0f * l - 1.0f) * (2.0f * l - 1.0f));
                        delta += clarity * 1.2f * dt * (0.3f + 0.7f * mid);
                    }
                    if (!fine.empty()) delta += texture * 1.5f * (l - fine[i]);
                    for (int k = 0; k < 3; ++k) d[k] += delta;
                }
                for (int k = 0; k < 3; ++k) d[k] = clamp01(d[k]);
                if (vibrance != 0.0f || saturation != 0.0f) {
                    float hh, sat, v;
                    rgbToHsv(d[0], d[1], d[2], hh, sat, v);
                    // Vibrance boosts muted colours more than saturated ones and goes easier on
                    // skin tones (orange hues), like Lightroom's.
                    float vib = vibrance;
                    if (vib > 0) {
                        const float skin = 1.0f - 0.5f * smooth(0.0f, 0.04f, hh) * (1.0f - smooth(0.1f, 0.16f, hh));
                        vib *= (1.0f - sat) * skin;
                    }
                    const float m = std::max(0.0f, (1.0f + saturation) * (1.0f + vib));
                    const float l = luma(d);
                    for (int k = 0; k < 3; ++k) d[k] = clamp01(l + (d[k] - l) * m);
                }
            }
        });
        applyFactor(*this, *src, *img, in[1]);
        out[0] = Value(ImagePtr(img));
    }

private:
    ImagePtr evaluateLinear(const ImagePtr& src, const Value& facIn) const {
        const float temp = paramF(1) / 100, tint = paramF(2) / 100, stops = paramF(3);
        const float contrast = paramF(4) / 100, highlights = paramF(5) / 100, shadows = paramF(6) / 100;
        const float whites = paramF(7) / 100, blacks = paramF(8) / 100;
        const float texture = paramF(9) / 100, clarity = paramF(10) / 100, dehaze = paramF(11) / 100;
        const float vibrance = paramF(12) / 100, saturation = paramF(13) / 100;
        const int w = src->w, h = src->h;
        const float longEdge = float(std::max(w, h));

        // White balance adapts the assumed light to D65 (CAT16); exposure is a plain multiply.
        colorsci::Mat3 wb;
        colorsci::whiteBalanceMatrix(temp, tint, wb);
        const bool useWb = temp != 0.0f || tint != 0.0f;
        const float gain = std::exp2(stops);
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            if (useWb) colorsci::mul(wb, s, d);
            else d[0] = s[0], d[1] = s[1], d[2] = s[2];
            for (int k = 0; k < 3; ++k) d[k] *= gain;
            d[3] = s[3];
        });

        // Haze is additive light, so removing it is most accurate on linear values.
        if (dehaze != 0.0f) dehazeImage(*img, dehaze, longEdge, 0.1f);

        // Tone: every slider becomes a gain in stops, applied as a ratio to RGB so hue and
        // saturation hold. Contrast bends the exposure scale around middle grey (softly limited,
        // so it can't push highlights without bound). Highlights and Shadows form a tone
        // equalizer: their bands are read from an edge-aware smoothed luminance (a guided filter
        // on log luminance, exposure-independent like darktable's EIGF), so whole regions move
        // together and local contrast survives. Whites and Blacks follow each pixel's own level,
        // like moving the end points of a curve.
        if (contrast != 0.0f || highlights != 0.0f || shadows != 0.0f || whites != 0.0f || blacks != 0.0f) {
            const std::vector<float> ev = logLuminance(*img);
            std::vector<float> mask;
            if (highlights != 0.0f || shadows != 0.0f) mask = fastGuidedSmooth(ev, w, h, std::max(longEdge * 0.02f, 1.0f), 0.5f);
            const float slope = 1.0f + contrast * (contrast > 0 ? 0.6f : 0.45f);
            const float hA = highlights * (highlights < 0 ? 1.5f : 1.0f), sA = shadows * (shadows > 0 ? 2.0f : 1.5f);
            const float wA = whites, bA = blacks * 1.5f;
            parallelFor(h, [&](int y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = size_t(y) * w + x;
                    const float e = ev[i];
                    float g = 0.0f;
                    if (contrast != 0.0f) g += (slope - 1.0f) * 3.0f * std::tanh((e - kMidGreyEv) / 3.0f);
                    if (!mask.empty()) {
                        const float em = mask[i];
                        g += hA * smooth(-3.0f, -0.5f, em) + sA * (1.0f - smooth(-6.0f, -2.5f, em));
                    }
                    const float ep = e + g;
                    g += wA * smooth(-1.5f, 1.0f, ep) + bA * (1.0f - smooth(-9.0f, -4.5f, ep));
                    const float m = std::exp2(g);
                    float* d = img->pixel(i);
                    for (int k = 0; k < 3; ++k) d[k] *= m;
                }
            });
        }

        // Local contrast in stops: detail = log luminance minus a smoothed copy, applied as a
        // ratio. Texture uses a small radius (fine detail), Clarity a large edge-aware one weighted
        // toward the midtones. Radii are relative to the image so the preview matches the export.
        if (clarity != 0.0f || texture != 0.0f) {
            const std::vector<float> L = logLuminance(*img);
            std::vector<float> coarse, fine;
            if (clarity != 0.0f) coarse = fastGuidedSmooth(L, w, h, std::max(longEdge * 0.012f, 1.0f), 0.1f);
            if (texture != 0.0f) {
                fine = L;
                const float sg = std::max(longEdge * 0.0025f, 0.7f);
                imageops::blurChannel(fine, w, h, sg, sg);
            }
            parallelFor(h, [&](int y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = size_t(y) * w + x;
                    float delta = 0.0f;
                    if (!coarse.empty()) {
                        float dt = L[i] - coarse[i];
                        dt /= 1.0f + 0.5f * std::fabs(dt);  // soft limit: no harsh overshoot
                        const float z = (L[i] - kMidGreyEv) / 2.5f;
                        delta += clarity * 0.8f * dt * (0.3f + 0.7f * std::exp(-0.5f * z * z));
                    }
                    if (!fine.empty()) {
                        float dt = L[i] - fine[i];
                        dt /= 1.0f + 0.5f * std::fabs(dt);
                        delta += texture * dt;
                    }
                    const float m = std::exp2(delta);
                    float* d = img->pixel(i);
                    for (int k = 0; k < 3; ++k) d[k] *= m;
                }
            });
        }

        // Vibrance and Saturation scale Oklch chroma, so lightness and hue stay put. Vibrance
        // favours muted colours and, when boosting, goes easy on skin (orange) hues.
        if (vibrance != 0.0f || saturation != 0.0f) {
            parallelFor(h, [&](int y) {
                for (int x = 0; x < w; ++x) {
                    float* d = img->pixel(size_t(y) * w + x);
                    float lab[3];
                    colorsci::rgbToOklab(d, lab);
                    const float C = std::sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
                    float vib = vibrance;
                    if (vib > 0.0f) {
                        const float hue = std::atan2(lab[2], lab[1]) * 57.29578f;
                        const float skin = 1.0f - 0.5f * smooth(20.0f, 40.0f, hue) * (1.0f - smooth(75.0f, 95.0f, hue));
                        vib *= (1.0f - smooth(0.0f, 0.2f, C)) * skin;
                    }
                    const float m = std::max(0.0f, (1.0f + saturation) * (1.0f + vib));
                    lab[1] *= m, lab[2] *= m;
                    colorsci::oklabToRgb(lab, d);
                }
            });
        }
        finishLinear(*img);
        applyFactor(*this, *src, *img, facIn);
        return img;
    }

    // Dark channel prior (He et al.): haze lifts the darkest channel of every patch toward the
    // airlight colour. Positive amounts remove that veil, negative ones add haze.
    // minAir: the airlight's floor (0.3 is mid grey in encoded values; linear ones need less).
    static void dehazeImage(Image& img, float amount, float longEdge, float minAir) {
        const int w = img.w, h = img.h;
        const size_t n = size_t(w) * h;
        std::vector<float> dark(n);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const float* p = img.pixel(size_t(y) * w + x);
                dark[size_t(y) * w + x] = std::clamp(std::min({p[0], p[1], p[2]}), 0.0f, 1.0f);
            }
        });
        const float sg = std::max(longEdge * 0.01f, 1.0f);
        imageops::blurChannel(dark, w, h, sg, sg);
        // Airlight: average colour of the haziest 0.1% of pixels.
        std::vector<float> sorted = dark;
        const size_t top = std::max<size_t>(1, n / 1000);
        std::nth_element(sorted.begin(), sorted.begin() + (n - top), sorted.end());
        const float thresh = sorted[n - top];
        double acc[3] = {0, 0, 0};
        size_t cnt = 0;
        for (size_t i = 0; i < n; ++i)
            if (dark[i] >= thresh) {
                const float* p = img.pixel(i);
                for (int k = 0; k < 3; ++k) acc[k] += std::clamp(p[k], 0.0f, 1.0f);
                ++cnt;
            }
        float A[3];
        for (int k = 0; k < 3; ++k) A[k] = std::max(float(acc[k] / double(std::max<size_t>(cnt, 1))), minAir);
        const float amax = std::max({A[0], A[1], A[2]});
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                float* p = img.pixel(i);
                if (amount > 0) {
                    const float t = std::max(1.0f - 0.95f * amount * dark[i] / amax, 0.25f);
                    for (int k = 0; k < 3; ++k) p[k] = (p[k] - A[k]) / t + A[k];
                } else {
                    // Thicker haze where the scene is already hazy (farther away).
                    const float hz = -amount * (0.35f + 0.4f * dark[i]);
                    for (int k = 0; k < 3; ++k) p[k] += (A[k] - p[k]) * hz;
                }
            }
        });
    }
};

// ---------------------------------------------------------------- Color Mixer

// Lightroom's eight colour bands, by centre hue in degrees.
const char* const kBandNames[8] = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
const float kBandHue[8] = {0.0f, 30.0f, 60.0f, 120.0f, 180.0f, 225.0f, 270.0f, 315.0f};

// Weights of the bands for a hue (0..360): the two neighbouring bands share it with a smooth
// crossfade, so the weights always sum to 1 and adjustments blend without seams.
void bandWeights(float hueDeg, float wgt[8]) {
    for (int i = 0; i < 8; ++i) wgt[i] = 0.0f;
    hueDeg = std::fmod(std::fmod(hueDeg, 360.0f) + 360.0f, 360.0f);
    for (int i = 0; i < 8; ++i) {
        const int j = (i + 1) % 8;
        const float a = kBandHue[i], b = j == 0 ? 360.0f : kBandHue[j];
        if (hueDeg >= a && hueDeg < b) {
            const float t = smooth(0.0f, 1.0f, (hueDeg - a) / (b - a));
            wgt[i] = 1.0f - t;
            wgt[j] = t;
            return;
        }
    }
    wgt[0] = 1.0f;
}

// The bands' centres as Oklch hues: the Oklch hue of each band's pure sRGB colour, so a band
// still selects the colours its name says.
const float* bandHuesOklch() {
    static const auto hues = [] {
        std::array<float, 8> a{};
        for (int i = 0; i < 8; ++i) {
            float r, g, b;
            hsvToRgb(kBandHue[i] / 360.0f, 1.0f, 1.0f, r, g, b);
            a[size_t(i)] = colorsci::oklabHueOfSrgb(r, g, b);
        }
        return a;
    }();
    return hues.data();
}

// bandWeights() for band centres that don't start at 0 degrees. Also returns, per band, the
// distance to the neighbouring centres, which scales the Hue sliders.
void bandWeightsAt(float hueDeg, const float* centre, float wgt[8]) {
    for (int i = 0; i < 8; ++i) wgt[i] = 0.0f;
    for (int i = 0; i < 8; ++i) {
        const int j = (i + 1) % 8;
        float a = centre[i], b = centre[j];
        if (b <= a) b += 360.0f;
        float h = hueDeg;
        if (h < a) h += 360.0f;
        if (h >= a && h < b) {
            const float t = smooth(0.0f, 1.0f, (h - a) / (b - a));
            wgt[i] = 1.0f - t;
            wgt[j] = t;
            return;
        }
    }
    wgt[0] = 1.0f;
}

float hueGap(const float* centre, int from, int to) {
    float d = std::fabs(centre[to] - centre[from]);
    return d > 180.0f ? 360.0f - d : d;
}

std::vector<ParamDesc> mixerParams() {
    std::vector<ParamDesc> p{ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f)};
    for (const char* what : {"Hue", "Saturation", "Luminance"})
        for (const char* band : kBandNames) p.push_back(ParamDesc::Float(std::string(band) + " " + what, 0.0f, -100.0f, 100.0f));
    return p;
}

class ColorMixerNode : public Node {
public:
    NODELAB_NODE({"color.color_mixer", "Color Mixer", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  mixerParams(), false, true})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        float hue[8], sat[8], lum[8];
        for (int i = 0; i < 8; ++i) hue[i] = paramF(1 + i) / 100, sat[i] = paramF(9 + i) / 100, lum[i] = paramF(17 + i) / 100;
        if (ctx.linear()) {
            out[0] = Value(evaluateLinear(src, in[1], hue, sat, lum));
            return;
        }
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            float h, sv, v;
            rgbToHsv(clamp01(s[0]), clamp01(s[1]), clamp01(s[2]), h, sv, v);
            float wgt[8];
            bandWeights(h * 360.0f, wgt);
            float dh = 0, ds = 0, dl = 0;
            for (int i = 0; i < 8; ++i) dh += wgt[i] * hue[i], ds += wgt[i] * sat[i], dl += wgt[i] * lum[i];
            // Greys have no hue, so the bands fade out as colour does.
            const float colourful = smooth(0.0f, 0.15f, sv * v);
            float r, g, b;
            // Hue +-100 moves about halfway to the neighbouring band.
            hsvToRgb(h + dh * colourful * 30.0f / 360.0f, sv, v, r, g, b);
            float c[3] = {r, g, b};
            const float l = luminance(r, g, b), m = std::max(0.0f, 1.0f + ds * colourful);
            // Luminance scales in linear light (-100 is about one stop down) so hue and
            // saturation hold.
            const float lm = std::exp2(dl * colourful);
            for (int k = 0; k < 3; ++k) {
                float cc = l + (c[k] - l) * m;
                d[k] = clamp01(linearToSrgb(srgbToLinear(std::max(cc, 0.0f)) * lm));
            }
            d[3] = s[3];
        });
        applyFactor(*this, *src, *img, in[1]);
        out[0] = Value(ImagePtr(img));
    }

private:
    // The same sliders in Oklch: hue moves along the perceptual hue circle, saturation scales
    // chroma, and luminance is an exposure change of the band (-100 is about one stop down).
    ImagePtr evaluateLinear(const ImagePtr& src, const Value& facIn, const float* hue, const float* sat, const float* lum) const {
        bool any = false;
        for (int i = 0; i < 8; ++i) any |= hue[i] != 0.0f || sat[i] != 0.0f || lum[i] != 0.0f;
        if (!any) return src;
        const float* centre = bandHuesOklch();
        // Hue +-100 moves about halfway to the neighbouring band on that side.
        float shift[8];
        for (int i = 0; i < 8; ++i)
            shift[i] = hue[i] * 0.5f * (hue[i] > 0 ? hueGap(centre, i, (i + 1) % 8) : hueGap(centre, i, (i + 7) % 8));
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            float L, C, hDeg;
            toOklch(s, L, C, hDeg);
            float wgt[8];
            bandWeightsAt(hDeg, centre, wgt);
            float dh = 0, ds = 0, dl = 0;
            for (int i = 0; i < 8; ++i) dh += wgt[i] * shift[i], ds += wgt[i] * sat[i], dl += wgt[i] * lum[i];
            // Greys have no hue, so the bands fade out as colour does.
            const float colourful = smooth(0.0f, 0.04f, C);
            fromOklch(L, C * std::max(0.0f, 1.0f + ds * colourful), hDeg + dh * colourful, d);
            const float lm = std::exp2(dl * colourful);
            for (int k = 0; k < 3; ++k) d[k] *= lm;
            d[3] = s[3];
        });
        finishLinear(*img);
        applyFactor(*this, *src, *img, facIn);
        return img;
    }
};

// ---------------------------------------------------------------- Color Grading

class ColorGradingNode : public Node {
public:
    NODELAB_NODE({"color.color_grading", "Color Grading", "Color",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Shadows Hue", 220.0f, 0.0f, 360.0f), ParamDesc::Float("Shadows Saturation", 0.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Shadows Luminance", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Midtones Hue", 40.0f, 0.0f, 360.0f), ParamDesc::Float("Midtones Saturation", 0.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Midtones Luminance", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Highlights Hue", 45.0f, 0.0f, 360.0f), ParamDesc::Float("Highlights Saturation", 0.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Highlights Luminance", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Global Hue", 0.0f, 0.0f, 360.0f), ParamDesc::Float("Global Saturation", 0.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Global Luminance", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Blending", 50.0f, 0.0f, 100.0f), ParamDesc::Float("Balance", 0.0f, -100.0f, 100.0f)},
                  false, true})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        if (ctx.linear()) {
            out[0] = Value(evaluateLinear(src, in[1]));
            return;
        }
        // Per zone (shadows, midtones, highlights, global): a luma-neutral tint and a lift.
        float tint[4][3], lift[4];
        for (int z = 0; z < 4; ++z) {
            float r, g, b;
            hsvToRgb(paramF(1 + z * 3) / 360.0f, 1.0f, 1.0f, r, g, b);
            const float l = luminance(r, g, b), amt = paramF(2 + z * 3) / 100 * 0.25f;
            tint[z][0] = (r - l) * amt, tint[z][1] = (g - l) * amt, tint[z][2] = (b - l) * amt;
            lift[z] = paramF(3 + z * 3) / 100 * 0.25f;
        }
        // Balance moves the split between shadows and highlights; Blending widens the overlap.
        const float pivot = 0.5f - paramF(14) / 100 * 0.3f;
        const float k = 1.0f + 3.0f * (1.0f - paramF(13) / 100);
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            const float l = clamp01(luminance(s[0], s[1], s[2]));
            float wz[4];
            wz[0] = std::pow(1.0f - smooth(0.0f, 2.0f * pivot, l), k);
            wz[2] = std::pow(smooth(2.0f * pivot - 1.0f, 1.0f, l), k);
            wz[1] = std::max(0.0f, 1.0f - wz[0] - wz[2]);
            wz[3] = 1.0f;
            for (int c = 0; c < 3; ++c) {
                float v = s[c];
                for (int z = 0; z < 4; ++z) v += (tint[z][c] + lift[z]) * wz[z];
                d[c] = clamp01(v);
            }
            d[3] = s[3];
        });
        applyFactor(*this, *src, *img, in[1]);
        out[0] = Value(ImagePtr(img));
    }

private:
    // Zones by Oklab lightness (perceptual, so the split sits where it looks right on scene
    // values); each zone shifts Oklab a/b toward its hue and moves lightness.
    ImagePtr evaluateLinear(const ImagePtr& src, const Value& facIn) const {
        bool any = false;
        float dir[4][2], amt[4], lift[4];
        for (int z = 0; z < 4; ++z) {
            float r, g, b;
            hsvToRgb(paramF(1 + z * 3) / 360.0f, 1.0f, 1.0f, r, g, b);
            const float h = colorsci::oklabHueOfSrgb(r, g, b) * 0.017453293f;
            dir[z][0] = std::cos(h), dir[z][1] = std::sin(h);
            amt[z] = paramF(2 + z * 3) / 100 * 0.08f;
            lift[z] = paramF(3 + z * 3) / 100 * 0.2f;
            any |= amt[z] != 0.0f || lift[z] != 0.0f;
        }
        if (!any) return src;
        const float pivot = 0.5f - paramF(14) / 100 * 0.3f;
        const float k = 1.0f + 3.0f * (1.0f - paramF(13) / 100);
        auto img = mapImage(*src, [&](int, int, const float* s, float* d) {
            float lab[3];
            colorsci::rgbToOklab(s, lab);
            const float l = clamp01(lab[0]);
            float wz[4];
            wz[0] = std::pow(1.0f - smooth(0.0f, 2.0f * pivot, l), k);
            wz[2] = std::pow(smooth(2.0f * pivot - 1.0f, 1.0f, l), k);
            wz[1] = std::max(0.0f, 1.0f - wz[0] - wz[2]);
            wz[3] = 1.0f;
            // Tints fade out toward black, so black stays neutral.
            const float tintFade = smooth(0.0f, 0.25f, l);
            float L = lab[0], a = lab[1], b = lab[2];
            for (int z = 0; z < 4; ++z) {
                a += dir[z][0] * amt[z] * wz[z] * tintFade;
                b += dir[z][1] * amt[z] * wz[z] * tintFade;
                L += lift[z] * wz[z];
            }
            const float o[3] = {std::max(L, 0.0f), a, b};
            colorsci::oklabToRgb(o, d);
            d[3] = s[3];
        });
        finishLinear(*img);
        applyFactor(*this, *src, *img, facIn);
        return img;
    }
};

}  // namespace

void registerDevelopNodes(NodeRegistry& r) {
    r.add<BasicNode>();
    r.add<ColorMixerNode>();
    r.add<ColorGradingNode>();
}
