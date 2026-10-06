#include "core/OutputSpace.h"

#include <algorithm>
#include <cmath>

#include "core/ColorMath.h"

namespace outspace {

namespace {

struct Primaries {
    double rx, ry, gx, gy, bx, by, wx, wy;
};
constexpr Primaries kD65_709{0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290};
constexpr Primaries kPrimaries[kCount] = {
    kD65_709,
    {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290},        // Display P3
    {0.64, 0.33, 0.21, 0.71, 0.15, 0.06, 0.3127, 0.3290},              // Adobe RGB (1998)
    {0.7347, 0.2653, 0.1596, 0.8404, 0.0366, 0.0001, 0.3457, 0.3585},  // ProPhoto (ROMM), D50
    {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290},        // Rec.2020
    {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290},        // Rec.2100
};
constexpr double kD50x = 0.3457, kD50y = 0.3585;

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}

Mat3 inverse(const Mat3& m) {
    const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0],
                 h = m[2][1], i = m[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    return {{{(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det},
             {(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det},
             {(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}}};
}

std::array<double, 3> xyzOf(double x, double y) { return {x / y, 1.0, (1.0 - x - y) / y}; }

// RGB to XYZ for a set of primaries, white mapping to Y = 1.
Mat3 rgbToXyz(const Primaries& p) {
    const auto r = xyzOf(p.rx, p.ry), g = xyzOf(p.gx, p.gy), b = xyzOf(p.bx, p.by), w = xyzOf(p.wx, p.wy);
    const Mat3 m{{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};
    const Mat3 inv = inverse(m);
    double s[3];
    for (int k = 0; k < 3; ++k) s[k] = inv[k][0] * w[0] + inv[k][1] * w[1] + inv[k][2] * w[2];
    Mat3 out = m;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) out[row][col] *= s[col];
    return out;
}

// Bradford chromatic adaptation between two white points (xy).
Mat3 bradford(double sx, double sy, double dx, double dy) {
    const Mat3 b{{{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
    const auto ws = xyzOf(sx, sy), wd = xyzOf(dx, dy);
    double cs[3], cd[3];
    for (int k = 0; k < 3; ++k) {
        cs[k] = b[k][0] * ws[0] + b[k][1] * ws[1] + b[k][2] * ws[2];
        cd[k] = b[k][0] * wd[0] + b[k][1] * wd[1] + b[k][2] * wd[2];
    }
    const Mat3 scale{{{cd[0] / cs[0], 0, 0}, {0, cd[1] / cs[1], 0}, {0, 0, cd[2] / cs[2]}}};
    return mul(inverse(b), mul(scale, b));
}

struct Tables {
    Mat3 from709[kCount], xyz50[kCount];
    Tables() {
        const Mat3 src = rgbToXyz(kD65_709);
        for (int s = 0; s < kCount; ++s) {
            const Primaries& p = kPrimaries[s];
            const Mat3 toXyz = rgbToXyz(p);
            // Rec.709 (D65) to the space's white, then into its RGB.
            const Mat3 adapt = bradford(kD65_709.wx, kD65_709.wy, p.wx, p.wy);
            from709[s] = mul(inverse(toXyz), mul(adapt, src));
            xyz50[s] = mul(bradford(p.wx, p.wy, kD50x, kD50y), toXyz);
        }
    }
};
const Tables& tables() {
    static const Tables t;
    return t;
}

}  // namespace

const Mat3& fromRec709(int space) { return tables().from709[valid(space) ? space : 0]; }
const Mat3& toXyzD50(int space) { return tables().xyz50[valid(space) ? space : 0]; }

float encode(int space, float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    switch (space) {
        case AdobeRGB: return std::pow(v, 256.0f / 563.0f);
        case ProPhoto: return v < 1.0f / 512.0f ? 16.0f * v : std::pow(v, 1.0f / 1.8f);
        case Rec2020: return v < 0.018053968f ? 4.5f * v : 1.0992968f * std::pow(v, 0.45f) - 0.0992968f;
        default: return colormath::linearToSrgb(v);
    }
}

double decode(int space, double e) {
    e = std::clamp(e, 0.0, 1.0);
    switch (space) {
        case AdobeRGB: return std::pow(e, 563.0 / 256.0);
        case ProPhoto: return e < 16.0 / 512.0 ? e / 16.0 : std::pow(e, 1.8);
        case Rec2020: return e < 0.081242858 ? e / 4.5 : std::pow((e + 0.0992968) / 1.0992968, 1.0 / 0.45);
        default: return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
    }
}

float pqEncode(float nits) {
    constexpr double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0, c1 = 3424.0 / 4096.0,
                     c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    const double y = std::pow(std::clamp(double(nits) / 10000.0, 0.0, 1.0), m1);
    return float(std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2));
}

}  // namespace outspace
