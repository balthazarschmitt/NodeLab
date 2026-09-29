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

// YCbCr (Rec.709, full range): Y 0..1, Cb/Cr centered on 0.5.
inline void rgbToYCbCr(float r, float g, float b, float& y, float& cb, float& cr) {
    y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    cb = (b - y) / 1.8556f + 0.5f;
    cr = (r - y) / 1.5748f + 0.5f;
}
inline void yCbCrToRgb(float y, float cb, float cr, float& r, float& g, float& b) {
    cb -= 0.5f;
    cr -= 0.5f;
    r = y + 1.5748f * cr;
    b = y + 1.8556f * cb;
    g = (y - 0.2126f * r - 0.0722f * b) / 0.7152f;
}

// YUV (BT.601): Y 0..1, U about -0.436..0.436, V about -0.615..0.615.
inline void rgbToYuv(float r, float g, float b, float& y, float& u, float& v) {
    y = 0.299f * r + 0.587f * g + 0.114f * b;
    u = 0.492f * (b - y);
    v = 0.877f * (r - y);
}
inline void yuvToRgb(float y, float u, float v, float& r, float& g, float& b) {
    r = y + 1.140f * v;
    g = y - 0.395f * u - 0.581f * v;
    b = y + 2.032f * u;
}

// HSL with h, s, l in 0..1.
inline void rgbToHsl(float r, float g, float b, float& h, float& s, float& l) {
    float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) * 0.5f;
    float d = mx - mn;
    if (d < 1e-6f) {
        h = s = 0.0f;
        return;
    }
    s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    float v;
    rgbToHsv(r, g, b, h, v, v);  // same hue as HSV
}
inline void hslToRgb(float h, float s, float l, float& r, float& g, float& b) {
    float c = (1.0f - std::fabs(2.0f * l - 1.0f)) * s;
    // Convert through HSV: v = l + c/2, sv = c / v.
    float v = l + c * 0.5f;
    float sv = v > 1e-6f ? c / v : 0.0f;
    hsvToRgb(h, sv, v, r, g, b);
}

inline void xyzToLinearRgb(float x, float y, float z, float& r, float& g, float& b) {
    r = 3.2404542f * x - 1.5371385f * y - 0.4985314f * z;
    g = -0.9692660f * x + 1.8760108f * y + 0.0415560f * z;
    b = 0.0556434f * x - 0.2040259f * y + 1.0572252f * z;
}

// CIE 1931 2-degree color matching functions, multi-lobe Gaussian fit (Wyman, Sloan, Shirley 2013).
inline void cieXyz(float nm, float& x, float& y, float& z) {
    auto g = [](float v, float mu, float s1, float s2) {
        float t = (v - mu) / (v < mu ? s1 : s2);
        return std::exp(-0.5f * t * t);
    };
    x = 1.056f * g(nm, 599.8f, 37.9f, 31.0f) + 0.362f * g(nm, 442.0f, 16.0f, 26.7f) - 0.065f * g(nm, 501.1f, 20.4f, 26.2f);
    y = 0.821f * g(nm, 568.8f, 46.9f, 40.5f) + 0.286f * g(nm, 530.9f, 16.3f, 31.1f);
    z = 1.217f * g(nm, 437.0f, 11.8f, 36.0f) + 0.681f * g(nm, 459.0f, 26.0f, 13.8f);
}

// Color of monochromatic light (nm), as displayable sRGB. Out-of-gamut parts are clipped; light
// outside ~380..780 nm fades to black, as it does for the eye.
inline void wavelengthToRgb(float nm, float& r, float& g, float& b) {
    float x, y, z;
    cieXyz(nm, x, y, z);
    xyzToLinearRgb(x, y, z, r, g, b);
    const float k = 1.0f / 1.6f;  // keeps the brightest (yellow-green) wavelengths just below clipping
    r = linearToSrgb(std::clamp(r * k, 0.0f, 1.0f));
    g = linearToSrgb(std::clamp(g * k, 0.0f, 1.0f));
    b = linearToSrgb(std::clamp(b * k, 0.0f, 1.0f));
}

// Color of an ideal black body at temperature (Kelvin), normalized so the brightest channel is 1.
inline void blackbodyToRgb(float kelvin, float& r, float& g, float& b) {
    double X = 0, Y = 0, Z = 0;
    const double c2 = 1.4388e-2;  // second radiation constant (m*K)
    for (int nm = 380; nm <= 780; nm += 5) {
        double lambda = nm * 1e-9;
        double planck = 1.0 / (std::pow(lambda, 5.0) * (std::exp(c2 / (lambda * kelvin)) - 1.0));
        float x, y, z;
        cieXyz(float(nm), x, y, z);
        X += planck * x;
        Y += planck * y;
        Z += planck * z;
    }
    float lr, lg, lb;
    xyzToLinearRgb(float(X / Y), 1.0f, float(Z / Y), lr, lg, lb);
    lr = std::max(lr, 0.0f), lg = std::max(lg, 0.0f), lb = std::max(lb, 0.0f);
    float m = std::max({lr, lg, lb, 1e-6f});
    r = linearToSrgb(lr / m);
    g = linearToSrgb(lg / m);
    b = linearToSrgb(lb / m);
}

}  // namespace colormath
