#pragma once
#include <algorithm>
#include <cmath>

// Color space helpers. RGB is sRGB-encoded 0..1 (what the app works in).
namespace colormath {

// HSV with h, s, v in 0..1 (h wraps).
inline void rgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    float d = mx - mn;
    v = mx;
    s = mx > 1e-6f ? d / mx : 0.0f;
    if (d < 1e-6f) {
        h = 0.0f;
        return;
    }
    if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h /= 6.0f;
}

inline void hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    h = h - std::floor(h);
    float f = h * 6.0f;
    int i = int(f) % 6;
    f -= std::floor(f);
    float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i) {
        case 0: r = v, g = t, b = p; break;
        case 1: r = q, g = v, b = p; break;
        case 2: r = p, g = v, b = t; break;
        case 3: r = p, g = q, b = v; break;
        case 4: r = t, g = p, b = v; break;
        default: r = v, g = p, b = q; break;
    }
}

inline float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(std::max(c, 0.0f), 1.0f / 2.4f) - 0.055f;
}

// CIE Lab (D65). Normalized for wires: L in 0..1 (L*/100), a and b roughly -1..1 (a*/128, b*/128).
inline void rgbToLab(float r, float g, float b, float& L, float& A, float& B) {
    float lr = srgbToLinear(r), lg = srgbToLinear(g), lb = srgbToLinear(b);
    float x = (0.4124564f * lr + 0.3575761f * lg + 0.1804375f * lb) / 0.95047f;
    float y = (0.2126729f * lr + 0.7151522f * lg + 0.0721750f * lb);
    float z = (0.0193339f * lr + 0.1191920f * lg + 0.9503041f * lb) / 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.0f / 116.0f; };
    float fx = f(x), fy = f(y), fz = f(z);
    L = (116.0f * fy - 16.0f) / 100.0f;
    A = 500.0f * (fx - fy) / 128.0f;
    B = 200.0f * (fy - fz) / 128.0f;
}

inline void labToRgb(float L, float A, float B, float& r, float& g, float& b) {
    float fy = (L * 100.0f + 16.0f) / 116.0f;
    float fx = fy + A * 128.0f / 500.0f;
    float fz = fy - B * 128.0f / 200.0f;
    auto finv = [](float t) { return t * t * t > 0.008856f ? t * t * t : (t - 16.0f / 116.0f) / 7.787f; };
    float x = finv(fx) * 0.95047f, y = finv(fy), z = finv(fz) * 1.08883f;
    float lr = 3.2404542f * x - 1.5371385f * y - 0.4985314f * z;
    float lg = -0.9692660f * x + 1.8760108f * y + 0.0415560f * z;
    float lb = 0.0556434f * x - 0.2040259f * y + 1.0572252f * z;
    r = linearToSrgb(lr);
    g = linearToSrgb(lg);
    b = linearToSrgb(lb);
}

}  // namespace colormath
