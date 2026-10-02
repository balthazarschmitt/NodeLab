#include "core/ColorManagement.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/ColorMath.h"
#include "core/Parallel.h"

nlohmann::json ColorManagement::toJson() const {
    return {{"workingSpace", linear ? "linear" : "legacy"},
            {"viewTransform", colormgmt::kViewNames[std::clamp(view, 0, 2)]},
            {"look", colormgmt::kLookNames[std::clamp(look, 0, 2)]},
            {"exposure", exposure},
            {"gamma", gamma}};
}

ColorManagement ColorManagement::fromJson(const nlohmann::json& j) {
    ColorManagement cm;
    if (!j.is_object()) return cm;
    auto str = [&](const char* k) { return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); };
    auto num = [&](const char* k, float def) { return j.contains(k) && j[k].is_number() ? j[k].get<float>() : def; };
    cm.linear = str("workingSpace") == "linear";
    // Stored by name, like the Enum params' spirit of never renumbering saved choices.
    for (int i = 0; i < 3; ++i) {
        if (str("viewTransform") == colormgmt::kViewNames[i]) cm.view = i;
        if (str("look") == colormgmt::kLookNames[i]) cm.look = i;
    }
    cm.exposure = num("exposure", 0.0f);
    cm.gamma = std::max(num("gamma", 1.0f), 0.01f);
    return cm;
}

namespace colormgmt {

namespace {

using M3 = float[3][3];

void mul(const M3 m, const float v[3], float o[3]) {
    for (int r = 0; r < 3; ++r) o[r] = m[r][0] * v[0] + m[r][1] * v[1] + m[r][2] * v[2];
}

// Rec.709 <-> Rec.2020 (ITU-R BT.2087), both linear with a D65 white.
constexpr M3 k709To2020 = {{0.6274039f, 0.3292830f, 0.0433131f},
                           {0.0690973f, 0.9195404f, 0.0113623f},
                           {0.0163914f, 0.0880133f, 0.8955953f}};
constexpr M3 k2020To709 = {{1.6604910f, -0.5876411f, -0.0728499f},
                           {-0.1245505f, 1.1328999f, -0.0083494f},
                           {-0.0181508f, -0.1005789f, 1.1187297f}};

// AgX as in Blender 4 (Eary Chow's Rec.2020 variant), in the analytic form Filament and three.js
// use: the inset matrix pulls colours toward white so bright saturated lights desaturate
// gracefully, and the outset restores some purity afterwards. Rows sum to 1, so greys stay grey.
constexpr M3 kAgxInset = {{0.856627153315983f, 0.0951212405381588f, 0.0482516061458583f},
                          {0.137318972929847f, 0.761241990602591f, 0.101439036467562f},
                          {0.11189821299995f, 0.0767994186031903f, 0.811302368396859f}};
constexpr M3 kAgxOutset = {{1.1271005818144368f, -0.11060664309660323f, -0.016493938717834573f},
                           {-0.1413297634984383f, 1.157823702216272f, -0.016493938717834257f},
                           {-0.14132976349843826f, -0.11060664309660294f, 1.2519364065950405f}};
// log2 range: 10 stops below and 6.5 above middle grey (0.18).
constexpr float kAgxMinEv = -12.47393f, kAgxMaxEv = 4.026069f;

void agx(const float in[3], int look, float out[3]) {
    float c[3], v[3];
    mul(k709To2020, in, c);
    mul(kAgxInset, c, v);
    for (float& x : v) {
        x = (std::log2(std::max(x, 1e-10f)) - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv);
        x = agxContrast(std::clamp(x, 0.0f, 1.0f));
    }
    if (look != ColorManagement::None) {
        // ASC CDL looks from the AgX reference; Blender ships the same Punchy and Greyscale.
        const float luma = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
        const float power = look == ColorManagement::Punchy ? 1.35f : 1.0f;
        const float sat = look == ColorManagement::Punchy ? 1.4f : 0.0f;
        for (float& x : v) x = luma + sat * (std::pow(std::max(x, 0.0f), power) - luma);
    }
    mul(kAgxOutset, v, c);
    for (float& x : c) x = std::pow(std::max(x, 0.0f), 2.2f);  // the sigmoid's output is gamma 2.2
    mul(k2020To709, c, out);
}

}  // namespace

float agxContrast(float x) {
    // 6th-order polynomial fit of AgX's default contrast curve (Benjamin Wrensch).
    const float x2 = x * x, x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}

void viewTransform(const ColorManagement& cm, const float in[3], float out[3]) {
    const float m = std::exp2(cm.exposure);
    float s[3] = {in[0] * m, in[1] * m, in[2] * m};
    switch (cm.view) {
        case ColorManagement::AgX: {
            float lin[3];
            agx(s, cm.look, lin);
            for (int k = 0; k < 3; ++k) out[k] = colormath::linearToSrgb(std::clamp(lin[k], 0.0f, 1.0f));
            break;
        }
        case ColorManagement::Raw:
            for (int k = 0; k < 3; ++k) out[k] = std::clamp(s[k], 0.0f, 1.0f);
            break;
        default:
            for (int k = 0; k < 3; ++k) out[k] = colormath::linearToSrgb(std::clamp(s[k], 0.0f, 1.0f));
            break;
    }
    if (cm.gamma != 1.0f)
        for (int k = 0; k < 3; ++k) out[k] = std::pow(out[k], 1.0f / cm.gamma);
}

namespace {

// viewTransform for whole images: its log2 and pow calls (nine a pixel for AgX) come from tables
// with linear interpolation. The tables are computed in double, so they're accurate to about
// 1e-7, closer than viewTransform's own float maths (AgX's polynomial cancels terms of about 30).
// viewTransform stays the reference (tests compare the two) and serves single pixels.
struct Lut {
    float lo = 0, scale = 0;
    std::vector<float> v;
    template <class F>
    Lut(float lo_, float hi, int n, F f) : lo(lo_), scale(n / (hi - lo_)), v(n + 2) {
        for (int i = 0; i <= n + 1; ++i) v[i] = float(f(lo_ + (double(hi) - lo_) * i / n));
    }
    // x within [lo, hi].
    float operator()(float x) const {
        const float p = (x - lo) * scale;
        const int i = int(p);
        return v[i] + (p - float(i)) * (v[i + 1] - v[i]);
    }
};

// sRGB encoding of 0..1.
const Lut& srgbLut() {
    static const Lut l(0.0f, 1.0f, 16384, [](double x) {
        return x <= 0.0031308 ? x * 12.92 : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
    });
    return l;
}

// AgX's display gamma, for 0..2 (the outset rarely leaves it; beyond is computed).
const Lut& gamma22Lut() {
    static const Lut l(0.0f, 2.0f, 16384, [](double x) { return std::pow(x, 2.2); });
    return l;
}

// AgX's log2 encoding and contrast sigmoid in one, indexed by the float's bits: the exponent and
// the top 10 mantissa bits pick the entry, the remaining bits interpolate (linear in x within
// each step). It covers 2^-13..2^5, which holds AgX's range of 2^-12.47..2^4.03; below and above
// it the curve is flat. The two steps holding the range's ends (where the clamp bends the curve)
// are computed exactly.
struct AgxCurve {
    static constexpr int kMinExp = -13, kOctaves = 18, kSub = 10;
    uint32_t base = 0;
    std::vector<float> v;
    float lowV = 0, highV = 0;
    uint32_t kinkLo = 0, kinkHi = 0;
    static float exact(float xf) {
        double x = (std::log2(std::max(double(xf), 1e-10)) - kAgxMinEv) / (double(kAgxMaxEv) - kAgxMinEv);
        x = std::clamp(x, 0.0, 1.0);
        const double x2 = x * x, x4 = x2 * x2;  // agxContrast in double
        return float(15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232);
    }
    AgxCurve() {
        const float first = std::ldexp(1.0f, kMinExp);
        std::memcpy(&base, &first, 4);
        const int n = kOctaves << kSub;
        v.resize(n + 1);
        for (int i = 0; i <= n; ++i) {
            const uint32_t bits = base + (uint32_t(i) << (23 - kSub));
            float x;
            std::memcpy(&x, &bits, 4);
            v[i] = exact(x);
        }
        lowV = exact(0.0f);
        highV = exact(std::ldexp(1.0f, kMinExp + kOctaves));
        auto cell = [&](float ev) {
            const float x = std::exp2(ev);
            uint32_t bits;
            std::memcpy(&bits, &x, 4);
            return (bits - base) >> (23 - kSub);
        };
        kinkLo = cell(kAgxMinEv);
        kinkHi = cell(kAgxMaxEv);
    }
    float operator()(float x) const {
        uint32_t bits;
        std::memcpy(&bits, &x, 4);
        if (!(x > 0.0f) || bits < base) return lowV;  // also NaN
        const uint32_t off = bits - base;
        const uint32_t i = off >> (23 - kSub);
        if (i >= uint32_t(v.size() - 1)) return highV;
        if (i == kinkLo || i == kinkHi) return exact(x);
        const float f = float(off & ((1u << (23 - kSub)) - 1)) * (1.0f / float(1u << (23 - kSub)));
        return v[i] + f * (v[i + 1] - v[i]);
    }
};

const AgxCurve& agxCurve() {
    static const AgxCurve c;
    return c;
}

inline float clamp01(float x) { return x > 0.0f ? (x < 1.0f ? x : 1.0f) : 0.0f; }  // NaN -> 0

}  // namespace

ImagePtr displayImage(const ImagePtr& img, const ColorManagement& cm) {
    if (!img || !cm.linear) return img;
    auto out = std::make_shared<Image>(img->w, img->h);
    const float m = std::exp2(cm.exposure);
    const float invGamma = 1.0f / cm.gamma;
    const Lut& srgb = srgbLut();
    const Lut& g22 = gamma22Lut();
    const AgxCurve& curve = agxCurve();
    // 709 -> 2020 -> inset folded into one matrix; the outset comes before the gamma, so it stays.
    float in[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            in[r][c] = kAgxInset[r][0] * k709To2020[0][c] + kAgxInset[r][1] * k709To2020[1][c] + kAgxInset[r][2] * k709To2020[2][c];
    const float power = cm.look == ColorManagement::Punchy ? 1.35f : 1.0f;
    const float sat = cm.look == ColorManagement::Punchy ? 1.4f : 0.0f;
    parallelFor(img->h, [&](int y) {
        const float* s = img->pixel(size_t(y) * img->w);
        float* d = out->pixel(size_t(y) * img->w);
        for (int x = 0; x < img->w; ++x, s += 4, d += 4) {
            const float r = s[0] * m, g = s[1] * m, b = s[2] * m;
            float o[3];
            if (cm.view == ColorManagement::AgX) {
                float v[3];
                for (int k = 0; k < 3; ++k) v[k] = curve(in[k][0] * r + in[k][1] * g + in[k][2] * b);
                if (cm.look != ColorManagement::None) {
                    const float luma = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
                    for (float& c : v) c = luma + sat * (std::pow(std::max(c, 0.0f), power) - luma);
                }
                float e[3];
                mul(kAgxOutset, v, e);
                for (float& c : e) {
                    c = std::max(c, 0.0f);
                    c = c <= 2.0f ? g22(c) : std::pow(c, 2.2f);
                }
                mul(k2020To709, e, o);
                for (float& c : o) c = srgb(clamp01(c));
            } else if (cm.view == ColorManagement::Raw) {
                o[0] = clamp01(r), o[1] = clamp01(g), o[2] = clamp01(b);
            } else {
                o[0] = srgb(clamp01(r)), o[1] = srgb(clamp01(g)), o[2] = srgb(clamp01(b));
            }
            if (cm.gamma != 1.0f)
                for (float& c : o) c = std::pow(c, invGamma);
            d[0] = o[0], d[1] = o[1], d[2] = o[2], d[3] = s[3];
        }
    });
    return out;
}

}  // namespace colormgmt
