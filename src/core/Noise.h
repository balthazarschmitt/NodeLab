#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

// Deterministic procedural noise (no global state, safe to call from many threads).
namespace noise {

inline uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline uint32_t hash2(int x, int y, uint32_t seed) {
    return hash(uint32_t(x) * 0x9E3779B1U ^ hash(uint32_t(y) * 0x85EBCA77U ^ hash(seed)));
}
// Uniform float in [0, 1).
inline float hashFloat(int x, int y, uint32_t seed, uint32_t salt = 0) {
    return float(hash2(x, y, seed + salt * 0x68E31DA4U) >> 8) / 16777216.0f;
}

// The lattice cell of a floored coordinate. High octaves on wide images reach past the int
// range, where int(floor(x)) overflows (and x - x0 became huge, then NaN): wrap instead.
inline int latticeIndex(float floored) {
    return int(int64_t(std::fmax(std::fmin(floored, 9e18f), -9e18f)));
}

// The gradient directions: a lattice point's hash picks one of 65536 angles. Computed once with the
// expression perlin used to evaluate per call (float(i) / 65536 * 2pi through std::cos/sin), so
// the noise is bit-identical, at a quarter of the cost (8 sin/cos per sample were most of it).
struct GradientDir {
    float c, s;
};
inline const GradientDir* gradientTable() {
    static const std::vector<GradientDir> t = [] {
        std::vector<GradientDir> v(65536);
        for (uint32_t i = 0; i < 65536; ++i) {
            const float a = float(i) / 65536.0f * 6.2831853f;
            v[i] = {std::cos(a), std::sin(a)};
        }
        return v;
    }();
    return t.data();
}

// 2D gradient (Perlin-style) noise in roughly [-1, 1].
inline float perlin(float x, float y, uint32_t seed) {
    static const GradientDir* const dirs = gradientTable();
    const float xf = std::floor(x), yf = std::floor(y);
    const int x0 = latticeIndex(xf), y0 = latticeIndex(yf);
    const float fx = x - xf, fy = y - yf;
    auto grad = [&](int ix, int iy, float dx, float dy) {
        const GradientDir& g = dirs[hash2(ix, iy, seed) & 0xFFFF];
        return g.c * dx + g.s * dy;
    };
    auto fade = [](float t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    float u = fade(fx), v = fade(fy);
    float n00 = grad(x0, y0, fx, fy), n10 = grad(x0 + 1, y0, fx - 1, fy);
    float n01 = grad(x0, y0 + 1, fx, fy - 1), n11 = grad(x0 + 1, y0 + 1, fx - 1, fy - 1);
    float nx0 = n00 + (n10 - n00) * u, nx1 = n01 + (n11 - n01) * u;
    return (nx0 + (nx1 - nx0) * v) * 1.414f;
}

// Fractal sum of octaves, normalized to about [0, 1]. detail = number of extra octaves (fractional ok).
inline float fbm(float x, float y, float detail, float roughness, float lacunarity, uint32_t seed) {
    float sum = 0, amp = 1, norm = 0, freq = 1;
    int octaves = int(detail);
    for (int i = 0; i <= octaves; ++i) {
        sum += perlin(x * freq, y * freq, seed + i * 131) * amp;
        norm += amp;
        amp *= roughness;
        freq *= lacunarity;
    }
    float frac = detail - octaves;
    if (frac > 0) {
        sum += perlin(x * freq, y * freq, seed + (octaves + 1) * 131) * amp * frac;
        norm += amp * frac;
    }
    return 0.5f + 0.5f * sum / norm;
}

// Voronoi F1: distance to the nearest jittered cell point, plus that cell's id for coloring.
inline float voronoi(float x, float y, float randomness, uint32_t seed, int& cellX, int& cellY) {
    int xi = latticeIndex(std::floor(x)), yi = latticeIndex(std::floor(y));
    float best = 1e9f;
    cellX = xi, cellY = yi;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            int cx = xi + i, cy = yi + j;
            float px = cx + 0.5f + (hashFloat(cx, cy, seed, 1) - 0.5f) * randomness;
            float py = cy + 0.5f + (hashFloat(cx, cy, seed, 2) - 0.5f) * randomness;
            float d = std::hypot(px - x, py - y);
            if (d < best) {
                best = d;
                cellX = cx;
                cellY = cy;
            }
        }
    return best;
}

}  // namespace noise
