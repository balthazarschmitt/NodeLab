#include "core/ColorScience.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include "core/ColorMath.h"

namespace colorsci {

namespace {

using DMat3 = double[3][3];

void mulD(const DMat3 a, const DMat3 b, DMat3 o) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) o[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
}

void inverseD(const DMat3 m, DMat3 o) {
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    const double k = 1.0 / det;
    o[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * k;
    o[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
    o[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
    o[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * k;
    o[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
    o[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
    o[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * k;
    o[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
    o[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
}

// Linear Rec.709 -> XYZ (D65).
constexpr DMat3 kRgbToXyz = {{0.4124564, 0.3575761, 0.1804375},
                             {0.2126729, 0.7151522, 0.0721750},
                             {0.0193339, 0.1191920, 0.9503041}};
// CAT16 (Li et al. 2017, the CIECAM16 adaptation space).
constexpr DMat3 kCat16 = {{0.401288, 0.650173, -0.051461},
                          {-0.250268, 1.204414, 0.045854},
                          {-0.002079, 0.048952, 0.953127}};
constexpr double kD65x = 0.31271, kD65y = 0.32902;
constexpr float kD65Kelvin = 6504.0f;

void xyToXyz(double x, double y, double xyz[3]) {
    xyz[0] = x / y;
    xyz[1] = 1.0;
    xyz[2] = (1.0 - x - y) / y;
}

// The white point, as XYZ with Y = 1, that the sliders say the light was.
void sourceWhite(float temp, float tint, double xyz[3]) {
    // Temperature moves along the locus in mireds (even steps look even); tint moves across it.
    // Offsetting D65 by the locus' own change keeps 0, 0 exactly on D65, which sits a little off
    // the locus.
    const float mired0 = 1e6f / kD65Kelvin;
    const float kelvin = 1e6f / std::max(mired0 - temp * 80.0f, 20.0f);
    float u0, v0, u1, v1;
    planckianUv(kD65Kelvin, u0, v0);
    planckianUv(kelvin, u1, v1);
    // Normal to the locus at the new temperature, pointing toward green (+v).
    float ua, va, ub, vb;
    planckianUv(kelvin * 0.99f, ua, va);
    planckianUv(kelvin * 1.01f, ub, vb);
    float nu = -(vb - va), nv = ub - ua;
    const float len = std::sqrt(nu * nu + nv * nv);
    nu /= len, nv /= len;
    if (nv < 0) nu = -nu, nv = -nv;
    const double d = kD65x * -2.0 + 12.0 * kD65y + 3.0;
    double u = 4.0 * kD65x / d + (u1 - u0) + tint * 0.03 * nu;
    double v = 6.0 * kD65y / d + (v1 - v0) + tint * 0.03 * nv;
    const double den = 2.0 * u - 8.0 * v + 4.0;
    xyToXyz(3.0 * u / den, 2.0 * v / den, xyz);
}

}  // namespace

void mul(const Mat3 m, const float v[3], float o[3]) {
    const float a = v[0], b = v[1], c = v[2];
    for (int r = 0; r < 3; ++r) o[r] = m[r][0] * a + m[r][1] * b + m[r][2] * c;
}

float cbrt(float x) {
    const float a = std::fabs(x);
    // Zero, infinity, NaN and tiny values (where the first guess is poor) go to the library.
    if (!(a >= 1e-30f && a <= 1e30f)) return std::cbrt(x);
    // A first guess from the exponent bits (within about 5%), then two Halley steps in double,
    // each tripling the correct digits: about 1e-12 relative, so the float rounds as std::cbrt's.
    double y = std::bit_cast<float>(std::bit_cast<uint32_t>(a) / 3 + 709921077u);
    const double ad = a;
    for (int k = 0; k < 2; ++k) {
        const double y3 = y * y * y;
        y *= (y3 + 2.0 * ad) / (2.0 * y3 + ad);
    }
    return std::copysign(float(y), x);
}

void rgbToOklab(const float c[3], float lab[3]) {
    const float l = cbrt(0.4122214708f * c[0] + 0.5363325363f * c[1] + 0.0514459929f * c[2]);
    const float m = cbrt(0.2119034982f * c[0] + 0.6806995451f * c[1] + 0.1073969566f * c[2]);
    const float s = cbrt(0.0883024619f * c[0] + 0.2817188376f * c[1] + 0.6299787005f * c[2]);
    lab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    lab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    lab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

void oklabToRgb(const float lab[3], float c[3]) {
    const float l_ = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    const float m_ = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    const float s_ = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    const float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    c[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    c[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    c[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

float oklabHueOfSrgb(float r, float g, float b) {
    const float rgb[3] = {colormath::srgbToLinear(r), colormath::srgbToLinear(g), colormath::srgbToLinear(b)};
    float lab[3];
    rgbToOklab(rgb, lab);
    const float h = std::atan2(lab[2], lab[1]) * 57.29578f;
    return h < 0 ? h + 360.0f : h;
}

void planckianUv(float kelvin, float& u, float& v) {
    const double T = std::clamp(double(kelvin), 1000.0, 15000.0), T2 = T * T;
    u = float((0.860117757 + 1.54118254e-4 * T + 1.28641212e-7 * T2) / (1.0 + 8.42420235e-4 * T + 7.08145163e-7 * T2));
    v = float((0.317398726 + 4.22806245e-5 * T + 4.20481691e-8 * T2) / (1.0 - 2.89741816e-5 * T + 1.61456053e-7 * T2));
}

void whiteBalanceMatrix(float temp, float tint, Mat3 out) {
    double src[3], dst[3];
    sourceWhite(temp, tint, src);
    xyToXyz(kD65x, kD65y, dst);
    // von Kries scaling in the CAT16 cone space: the assumed light's white becomes D65's.
    double ls[3], ld[3];
    for (int r = 0; r < 3; ++r) {
        ls[r] = kCat16[r][0] * src[0] + kCat16[r][1] * src[1] + kCat16[r][2] * src[2];
        ld[r] = kCat16[r][0] * dst[0] + kCat16[r][1] * dst[1] + kCat16[r][2] * dst[2];
    }
    DMat3 D = {{ld[0] / ls[0], 0, 0}, {0, ld[1] / ls[1], 0}, {0, 0, ld[2] / ls[2]}};
    DMat3 catInv, xyzToRgb, t1, t2, t3, m;
    inverseD(kCat16, catInv);
    inverseD(kRgbToXyz, xyzToRgb);
    mulD(kCat16, kRgbToXyz, t1);
    mulD(D, t1, t2);
    mulD(catInv, t2, t3);
    mulD(xyzToRgb, t3, m);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r][c] = float(m[r][c]);
}

void whiteBalanceSource(float temp, float tint, float rgb[3]) {
    double xyz[3];
    sourceWhite(temp, tint, xyz);
    DMat3 xyzToRgb;
    inverseD(kRgbToXyz, xyzToRgb);
    for (int r = 0; r < 3; ++r) rgb[r] = float(xyzToRgb[r][0] * xyz[0] + xyzToRgb[r][1] * xyz[1] + xyzToRgb[r][2] * xyz[2]);
}

void compressToGamut(float c[3]) {
    const float lo = std::min({c[0], c[1], c[2]});
    if (lo >= 0.0f) return;
    const float y = std::max(0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2], 0.0f);
    if (y <= 0.0f) {
        c[0] = c[1] = c[2] = 0.0f;
        return;
    }
    // Largest t with y + t * (c - y) >= 0 in every channel.
    const float t = y / (y - lo);
    for (int k = 0; k < 3; ++k) c[k] = std::max(y + t * (c[k] - y), 0.0f);
}

}  // namespace colorsci
