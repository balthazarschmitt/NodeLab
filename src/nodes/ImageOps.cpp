#include "nodes/ImageOps.h"

#include <algorithm>
#include <cmath>

#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "core/Value.h"

namespace imageops {

// Box radii for three passes approximating a Gaussian of the given sigma.
std::vector<int> boxRadii(float sigma) {
    if (sigma < 0.3f) return {};
    const int n = 3;
    float wIdeal = std::sqrt(12.0f * sigma * sigma / n + 1.0f);
    int wl = int(std::floor(wIdeal));
    if (wl % 2 == 0) --wl;
    int wu = wl + 2;
    float mIdeal = (12.0f * sigma * sigma - n * wl * wl - 4.0f * n * wl - 3.0f * n) / (-4.0f * wl - 4.0f);
    int m = int(std::round(mIdeal));
    std::vector<int> r;
    for (int i = 0; i < n; ++i) r.push_back(((i < m ? wl : wu) - 1) / 2);
    return r;
}

namespace {

// One edge-clamped box pass along a line of `len` elements, each `n` contiguous floats (the
// interleaved channels of one pixel, or of a block of pixels). Reads src, writes dst (distinct).
// The running sum visits values in the same order as a straightforward clamped loop.
void boxLine(const float* src, float* dst, int len, int n, int radius, float* acc) {
    const float inv = 1.0f / float(2 * radius + 1);
    auto at = [&](int i) { return src + size_t(std::clamp(i, 0, len - 1)) * n; };
    std::fill(acc, acc + n, 0.0f);
    for (int i = -radius; i <= radius; ++i) {
        const float* s = at(i);
        for (int k = 0; k < n; ++k) acc[k] += s[k];
    }
    for (int i = 0; i < len; ++i) {
        float* d = dst + size_t(i) * n;
        for (int k = 0; k < n; ++k) d[k] = acc[k] * inv;
        const float *add = at(i + radius + 1), *sub = at(i - radius);
        for (int k = 0; k < n; ++k) acc[k] += add[k] - sub[k];
    }
}

// All box passes of one axis, done per line (or per block of columns) in scratch buffers, so the
// data is read and written once per axis instead of once per pass. The vertical axis works on
// blocks of columns: walking one column at a time touches a new cache line for every pixel.
void boxBlur(float* data, int w, int h, int ch, float sigmaX, float sigmaY) {
    const std::vector<int> rx = boxRadii(sigmaX), ry = boxRadii(sigmaY);
    auto run = [](const std::vector<int>& radii, std::vector<float>& a, std::vector<float>& b, int len, int n,
                  std::vector<float>& acc) {
        for (int r : radii) {
            if (r <= 0) continue;
            boxLine(a.data(), b.data(), len, n, r, acc.data());
            a.swap(b);
        }
    };
    if (!rx.empty()) {
        const size_t row = size_t(w) * ch;
        parallelFor(h, [&](int y) {
            std::vector<float> a(data + y * row, data + (y + 1) * row), b(row), acc(static_cast<size_t>(ch));
            run(rx, a, b, w, ch, acc);
            std::copy(a.begin(), a.end(), data + y * row);
        });
    }
    if (!ry.empty()) {
        constexpr int kBlock = 16;  // pixels per column block: 64 B (channel) or 256 B (RGBA) per row
        const int blocks = (w + kBlock - 1) / kBlock;
        parallelFor(blocks, [&](int bi) {
            const int x0 = bi * kBlock, bw = std::min(kBlock, w - x0), n = bw * ch;
            std::vector<float> a(size_t(h) * n), b(a.size()), acc(static_cast<size_t>(n));
            for (int y = 0; y < h; ++y) {
                const float* s = data + (size_t(y) * w + x0) * ch;
                std::copy(s, s + n, a.data() + size_t(y) * n);
            }
            run(ry, a, b, h, n, acc);
            for (int y = 0; y < h; ++y) {
                const float* s = a.data() + size_t(y) * n;
                std::copy(s, s + n, data + (size_t(y) * w + x0) * ch);
            }
        });
    }
}

}  // namespace

int blurReach(float sigma) {
    int r = 0;
    for (int b : boxRadii(sigma)) r += std::max(0, b);
    return r;
}

void blurImage(Image& img, float sigmaX, float sigmaY) {
    if (img.empty()) return;
    boxBlur(img.px.data(), img.w, img.h, 4, sigmaX, sigmaY);
}

void blurChannel(std::vector<float>& ch, int w, int h, float sigmaX, float sigmaY) {
    if (w <= 0 || h <= 0) return;
    boxBlur(ch.data(), w, h, 1, sigmaX, sigmaY);
}

namespace {

// 1D squared distance transform (Felzenszwalb & Huttenlocher), lower envelope of parabolas.
void dt1d(const double* f, double* d, int n, int* v, double* z) {
    int k = 0;
    v[0] = 0;
    z[0] = -1e30;
    z[1] = 1e30;
    auto inter = [&](int q, int p) { return ((f[q] + double(q) * q) - (f[p] + double(p) * p)) / (2.0 * q - 2.0 * p); };
    for (int q = 1; q < n; ++q) {
        double s = inter(q, v[k]);
        while (s <= z[k]) {
            --k;
            s = inter(q, v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e30;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) ++k;
        d[q] = double(q - v[k]) * (q - v[k]) + f[v[k]];
    }
}

}  // namespace

std::vector<float> distanceTransform(const std::vector<uint8_t>& mask, int w, int h) {
    const double inf = 1e20;
    std::vector<double> g(size_t(w) * h);
    for (size_t i = 0; i < g.size(); ++i) g[i] = mask[i] ? 0.0 : inf;
    parallelFor(w, [&](int x) {  // columns
        std::vector<double> f(h), d(h), z(h + 1);
        std::vector<int> v(h);
        for (int y = 0; y < h; ++y) f[y] = g[size_t(y) * w + x];
        dt1d(f.data(), d.data(), h, v.data(), z.data());
        for (int y = 0; y < h; ++y) g[size_t(y) * w + x] = d[y];
    });
    std::vector<float> out(g.size());
    parallelFor(h, [&](int y) {  // rows
        std::vector<double> f(w), d(w), z(w + 1);
        std::vector<int> v(w);
        for (int x = 0; x < w; ++x) f[x] = g[size_t(y) * w + x];
        dt1d(f.data(), d.data(), w, v.data(), z.data());
        for (int x = 0; x < w; ++x) out[size_t(y) * w + x] = float(std::sqrt(std::min(d[x], 1e12)));
    });
    return out;
}

int sharpenReach(float radius) { return blurReach(radius) + 2; }

void sharpenImage(Image& img, const SharpenSettings& s, bool linear) {
    const int w = img.w, h = img.h;
    if (img.empty() || !(s.amount > 0.0f) || !(s.radius > 0.0f)) return;
    const size_t n = size_t(w) * h;
    std::vector<float> P(n);
    parallelFor(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            const float* p = img.pixel(size_t(y) * w + x);
            float Y = luminance(p[0], p[1], p[2]);
            if (!std::isfinite(Y)) Y = 0.0f;
            P[size_t(y) * w + x] = linear ? colormath::linearToSrgb(std::max(Y, 0.0f)) : Y;
        }
    });
    std::vector<float> B = P;
    blurChannel(B, w, h, s.radius, s.radius);

    const float k = std::clamp(s.amount, 0.0f, 150.0f) / 100.0f * 2.0f;
    const float detail = std::clamp(s.detail, 0.0f, 100.0f) / 100.0f;
    // Masking: a step of contrast c blurred by sigma has a peak gradient of about 0.4 c / sigma,
    // so the gradient times sigma / 0.4 measures the edge's contrast whatever the radius.
    const float m = std::clamp(s.masking, 0.0f, 100.0f) / 100.0f;
    const float threshold = m * std::sqrt(m) * 0.25f;
    const float gradScale = s.radius / 0.4f * 0.5f;  // central differences span 2 pixels
    parallelFor(h, [&](int y) {
        const int ya = std::max(y - 1, 0), yb = std::min(y + 1, h - 1);
        for (int x = 0; x < w; ++x) {
            const int xa = std::max(x - 1, 0), xb = std::min(x + 1, w - 1);
            const size_t i = size_t(y) * w + x;
            float lo = P[i], hi = P[i];
            for (int yy = ya; yy <= yb; ++yy)
                for (int xx = xa; xx <= xb; ++xx) {
                    const float v = P[size_t(yy) * w + xx];
                    lo = std::min(lo, v), hi = std::max(hi, v);
                }
            float weight = 1.0f;
            if (threshold > 0.0f) {
                const float gx = B[size_t(y) * w + xb] - B[size_t(y) * w + xa];
                const float gy = B[size_t(yb) * w + x] - B[size_t(ya) * w + x];
                const float e = std::sqrt(gx * gx + gy * gy) * gradScale;
                const float t = std::clamp((e - threshold * 0.5f) / threshold, 0.0f, 1.0f);
                weight = t * t * (3.0f - 2.0f * t);
            }
            const float sharp = P[i] + k * weight * (P[i] - B[i]);
            const float held = std::clamp(sharp, lo, hi);
            const float out = held + detail * (sharp - held);
            const float delta = linear ? colormath::srgbToLinear(std::max(out, 0.0f)) - colormath::srgbToLinear(P[i]) : out - P[i];
            if (!std::isfinite(delta)) continue;
            float* p = img.pixel(i);
            for (int c = 0; c < 3; ++c) p[c] = std::max(p[c] + delta, 0.0f);
        }
    });
}

}  // namespace imageops
