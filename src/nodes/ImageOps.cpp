#include "nodes/ImageOps.h"

#include <algorithm>
#include <cmath>

#include "core/Parallel.h"

namespace imageops {

void sampleBilinear(const Image& img, float x, float y, float out[4], bool transparentOutside) {
    if (img.empty()) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    if (transparentOutside && (x < 0 || y < 0 || x > img.w || y > img.h)) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    x -= 0.5f;
    y -= 0.5f;
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    float fx = x - x0, fy = y - y0;
    auto px = [&](int xi, int yi) {
        xi = std::clamp(xi, 0, img.w - 1);
        yi = std::clamp(yi, 0, img.h - 1);
        return img.pixel(size_t(yi) * img.w + xi);
    };
    const float *a = px(x0, y0), *b = px(x0 + 1, y0), *c = px(x0, y0 + 1), *d = px(x0 + 1, y0 + 1);
    for (int k = 0; k < 4; ++k) {
        float top = a[k] + (b[k] - a[k]) * fx;
        float bot = c[k] + (d[k] - c[k]) * fx;
        out[k] = top + (bot - top) * fy;
    }
}

float sampleBilinear(const std::vector<float>& ch, int w, int h, float x, float y) {
    x -= 0.5f;
    y -= 0.5f;
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    float fx = x - x0, fy = y - y0;
    auto at = [&](int xi, int yi) {
        xi = std::clamp(xi, 0, w - 1);
        yi = std::clamp(yi, 0, h - 1);
        return ch[size_t(yi) * w + xi];
    };
    float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * fx;
    float bot = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * fx;
    return top + (bot - top) * fy;
}

namespace {

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

// One box pass over `count` lines of length `len`; element i of line l is at base(l) + i*stride.
// Edges clamp. `channels` interleaved values per element.
void boxPass(float* data, int lines, int len, size_t lineStride, size_t elemStride, int channels, int radius) {
    if (radius <= 0) return;
    parallelFor(lines, [&](int l) {
        std::vector<float> line(size_t(len) * channels);
        float* base = data + size_t(l) * lineStride;
        for (int i = 0; i < len; ++i)
            for (int c = 0; c < channels; ++c) line[size_t(i) * channels + c] = base[size_t(i) * elemStride + c];
        const float inv = 1.0f / float(2 * radius + 1);
        for (int c = 0; c < channels; ++c) {
            auto at = [&](int i) { return line[size_t(std::clamp(i, 0, len - 1)) * channels + c]; };
            float acc = 0;
            for (int i = -radius; i <= radius; ++i) acc += at(i);
            for (int i = 0; i < len; ++i) {
                base[size_t(i) * elemStride + c] = acc * inv;
                acc += at(i + radius + 1) - at(i - radius);
            }
        }
    });
}

}  // namespace

void blurImage(Image& img, float sigmaX, float sigmaY) {
    if (img.empty()) return;
    for (int r : boxRadii(sigmaX)) boxPass(img.px.data(), img.h, img.w, size_t(img.w) * 4, 4, 4, r);
    for (int r : boxRadii(sigmaY)) boxPass(img.px.data(), img.w, img.h, 4, size_t(img.w) * 4, 4, r);
}

void blurChannel(std::vector<float>& ch, int w, int h, float sigmaX, float sigmaY) {
    for (int r : boxRadii(sigmaX)) boxPass(ch.data(), h, w, size_t(w), 1, 1, r);
    for (int r : boxRadii(sigmaY)) boxPass(ch.data(), w, h, 1, size_t(w), 1, r);
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

}  // namespace imageops
