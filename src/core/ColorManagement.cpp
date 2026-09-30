#include "core/ColorManagement.h"

#include <algorithm>
#include <cmath>

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

ImagePtr displayImage(const ImagePtr& img, const ColorManagement& cm) {
    if (!img || !cm.linear) return img;
    auto out = std::make_shared<Image>(img->w, img->h);
    parallelFor(img->h, [&](int y) {
        for (int x = 0; x < img->w; ++x) {
            const size_t i = size_t(y) * img->w + x;
            const float* s = img->pixel(i);
            float* d = out->pixel(i);
            viewTransform(cm, s, d);
            d[3] = s[3];
        }
    });
    return out;
}

}  // namespace colormgmt
