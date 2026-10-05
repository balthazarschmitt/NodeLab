#pragma once
// core/Noise.h in GLSL, bit for bit in the hashes (lattice cells and cell colours must match).
inline const char* const kGlslNoise = R"(
uint nHash(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
uint nHash2(int x, int y, uint seed) { return nHash(uint(x) * 0x9E3779B1u ^ nHash(uint(y) * 0x85EBCA77u ^ nHash(seed))); }
float nHashFloat(int x, int y, uint seed, uint salt) {
    return float(nHash2(x, y, seed + salt * 0x68E31DA4u) >> 8) * (1.0 / 16777216.0);
}
float nGrad(int ix, int iy, float dx, float dy, uint seed) {
    float a = float(nHash2(ix, iy, seed) & 0xFFFFu) * (1.0 / 65536.0) * 6.2831853;
    return cos(a) * dx + sin(a) * dy;
}
float nFade(float t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
float nPerlin(float x, float y, uint seed) {
    float xf = floor(x), yf = floor(y);
    int x0 = int(xf), y0 = int(yf);
    float fx = x - xf, fy = y - yf;
    float u = nFade(fx), v = nFade(fy);
    float n00 = nGrad(x0, y0, fx, fy, seed), n10 = nGrad(x0 + 1, y0, fx - 1.0, fy, seed);
    float n01 = nGrad(x0, y0 + 1, fx, fy - 1.0, seed), n11 = nGrad(x0 + 1, y0 + 1, fx - 1.0, fy - 1.0, seed);
    float nx0 = n00 + (n10 - n00) * u, nx1 = n01 + (n11 - n01) * u;
    return (nx0 + (nx1 - nx0) * v) * 1.414;
}
float nFbm(float x, float y, float detail, float roughness, float lacunarity, uint seed) {
    float sum = 0.0, amp = 1.0, norm = 0.0, freq = 1.0;
    int octaves = int(detail);
    for (int i = 0; i <= octaves; ++i) {
        sum += nPerlin(x * freq, y * freq, seed + uint(i) * 131u) * amp;
        norm += amp;
        amp *= roughness;
        freq *= lacunarity;
    }
    float frac = detail - float(octaves);
    if (frac > 0.0) {
        sum += nPerlin(x * freq, y * freq, seed + uint(octaves + 1) * 131u) * amp * frac;
        norm += amp * frac;
    }
    return 0.5 + 0.5 * sum / norm;
}
float nVoronoi(float x, float y, float randomness, uint seed, out ivec2 cell) {
    int xi = int(floor(x)), yi = int(floor(y));
    float best = 1e9;
    cell = ivec2(xi, yi);
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            int cx = xi + i, cy = yi + j;
            precise float px = float(cx) + 0.5 + (nHashFloat(cx, cy, seed, 1u) - 0.5) * randomness;
            precise float py = float(cy) + 0.5 + (nHashFloat(cx, cy, seed, 2u) - 0.5) * randomness;
            float d = length(vec2(px - x, py - y));
            if (d < best) {
                best = d;
                cell = ivec2(cx, cy);
            }
        }
    return best;
}
)";
