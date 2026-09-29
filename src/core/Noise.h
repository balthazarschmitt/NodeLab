#pragma once
#include <cmath>
#include <cstdint>

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

// 2D gradient (Perlin-style) noise in roughly [-1, 1].
inline float perlin(float x, float y, uint32_t seed) {
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    float fx = x - x0, fy = y - y0;
    auto grad = [&](int ix, int iy, float dx, float dy) {
        uint32_t h = hash2(ix, iy, seed);
        float a = float(h & 0xFFFF) / 65536.0f * 6.2831853f;
        return std::cos(a) * dx + std::sin(a) * dy;
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
    int xi = int(std::floor(x)), yi = int(std::floor(y));
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
