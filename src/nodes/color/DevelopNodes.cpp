// Photo "develop" nodes modelled on Lightroom's panels: Basic, Color Mixer (HSL) and Color
// Grading. Sliders use Lightroom's -100..100 scale so numbers carry over. Like the other colour
// nodes the output is clamped to 0..1, but the stages in between stay unclamped so Highlights can
// pull back values that Exposure or White Balance pushed past white.
#include <cmath>

#include "core/ColorMath.h"
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
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
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

        if (dehaze != 0.0f) dehazeImage(*img, dehaze, longEdge);

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
    // Dark channel prior (He et al.): haze lifts the darkest channel of every patch toward the
    // airlight colour. Positive amounts remove that veil, negative ones add haze.
    static void dehazeImage(Image& img, float amount, float longEdge) {
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
        for (int k = 0; k < 3; ++k) A[k] = std::max(float(acc[k] / double(std::max<size_t>(cnt, 1))), 0.3f);
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
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        float hue[8], sat[8], lum[8];
        for (int i = 0; i < 8; ++i) hue[i] = paramF(1 + i) / 100, sat[i] = paramF(9 + i) / 100, lum[i] = paramF(17 + i) / 100;
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
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
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
};

}  // namespace

void registerDevelopNodes(NodeRegistry& r) {
    r.add<BasicNode>();
    r.add<ColorMixerNode>();
    r.add<ColorGradingNode>();
}
