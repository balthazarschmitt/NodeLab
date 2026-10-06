// Upright (Lightroom's Transform panel): straight lines found in the photo, and the camera
// rotation that makes the chosen ones vertical or horizontal. Guided Upright solves for lines
// drawn by hand; Auto, Level, Vertical and Full for lines found here.
#include <algorithm>
#include <cmath>

#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "nodes/transform/TransformNodes.h"

namespace perspective {

namespace {

using M3 = std::array<double, 9>;
M3 mul3(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    return r;
}
M3 rotation3(const double a[3]) {
    const M3 x = {1, 0, 0, 0, std::cos(a[0]), -std::sin(a[0]), 0, std::sin(a[0]), std::cos(a[0])};
    const M3 y = {std::cos(a[1]), 0, std::sin(a[1]), 0, 1, 0, -std::sin(a[1]), 0, std::cos(a[1])};
    const M3 z = {std::cos(a[2]), -std::sin(a[2]), 0, std::sin(a[2]), std::cos(a[2]), 0, 0, 0, 1};
    return mul3(z, mul3(y, x));
}

bool solve3x3(const double A[9], const double b[3], double x[3]) {
    const double det = A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
    if (!(std::fabs(det) > 1e-300)) return false;
    for (int k = 0; k < 3; ++k) {
        double M[9];
        std::copy(A, A + 9, M);
        for (int r = 0; r < 3; ++r) M[r * 3 + k] = b[r];
        x[k] = (M[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (M[3] * M[8] - M[5] * M[6]) + M[2] * (M[3] * M[7] - M[4] * M[6])) / det;
    }
    return std::isfinite(x[0]) && std::isfinite(x[1]) && std::isfinite(x[2]);
}

}  // namespace

// Each line's slope away from upright once the camera is turned by a (times its weight), and a
// small pull towards no rotation, which settles the angles the lines leave free (one vertical
// line says nothing about the horizon).
static int uprightResiduals(const std::vector<UprightLine>& lines, const double a[3], std::vector<double>& r) {
    const M3 R = rotation3(a);
    r.clear();
    for (const UprightLine& l : lines) {
        const double z0 = R[6] * l.x0 + R[7] * l.y0 + R[8], z1 = R[6] * l.x1 + R[7] * l.y1 + R[8];
        if (z0 < 1e-3 || z1 < 1e-3) {
            r.push_back(1.0 * l.weight);  // turned out of view
            continue;
        }
        const double px0 = (R[0] * l.x0 + R[1] * l.y0 + R[2]) / z0, py0 = (R[3] * l.x0 + R[4] * l.y0 + R[5]) / z0;
        const double px1 = (R[0] * l.x1 + R[1] * l.y1 + R[2]) / z1, py1 = (R[3] * l.x1 + R[4] * l.y1 + R[5]) / z1;
        const double len = std::max(std::hypot(px1 - px0, py1 - py0), 1e-9);
        r.push_back((l.vertical ? px1 - px0 : py1 - py0) / len * l.weight);
    }
    for (int k = 0; k < 3; ++k) r.push_back(1e-3 * a[k]);
    return int(r.size());
}

void solveUpright(const std::vector<UprightLine>& lines, const bool free[3], double ang[3]) {
    ang[0] = ang[1] = ang[2] = 0;
    if (lines.empty()) return;
    // Levenberg-Marquardt with a numeric Jacobian: three unknowns, a handful of residuals.
    double a[3] = {0, 0, 0};
    std::vector<double> r, r2;
    const int n = uprightResiduals(lines, a, r);
    std::vector<std::array<double, 3>> J(static_cast<size_t>(n));
    const auto cost = [&](const std::vector<double>& res) {
        double c = 0;
        for (int i = 0; i < n; ++i) c += res[size_t(i)] * res[size_t(i)];
        return c;
    };
    double c = cost(r), mu = 1e-3;
    for (int it = 0; it < 60 && c > 1e-20; ++it) {
        for (int k = 0; k < 3; ++k) {
            if (!free[k]) {
                for (int i = 0; i < n; ++i) J[size_t(i)][size_t(k)] = 0.0;
                continue;
            }
            double b[3] = {a[0], a[1], a[2]};
            b[k] += 1e-7;
            uprightResiduals(lines, b, r2);
            for (int i = 0; i < n; ++i) J[size_t(i)][size_t(k)] = (r2[size_t(i)] - r[size_t(i)]) / 1e-7;
        }
        double A[9] = {}, g[3] = {};
        for (int i = 0; i < n; ++i)
            for (int p = 0; p < 3; ++p) {
                g[p] -= J[size_t(i)][size_t(p)] * r[size_t(i)];
                for (int q = 0; q < 3; ++q) A[p * 3 + q] += J[size_t(i)][size_t(p)] * J[size_t(i)][size_t(q)];
            }
        bool improved = false;
        while (mu < 1e8) {
            double M[9], step[3];
            std::copy(A, A + 9, M);
            for (int p = 0; p < 3; ++p) M[p * 4] += mu * (1.0 + A[p * 4]);
            if (!solve3x3(M, g, step)) break;
            double b[3];
            for (int k = 0; k < 3; ++k) b[k] = free[k] ? std::clamp(a[k] + step[k], -0.8, 0.8) : 0.0;
            uprightResiduals(lines, b, r2);
            const double c2 = cost(r2);
            if (c2 < c) {
                std::copy(b, b + 3, a);
                r = r2;
                c = c2;
                mu = std::max(mu * 0.3, 1e-9);
                improved = true;
                break;
            }
            mu *= 10;
        }
        if (!improved) break;
    }
    std::copy(a, a + 3, ang);
}

// A small LSD (line segment detector, von Gioi et al.): pixels with a strong gradient are grown
// into regions of the same gradient direction, and long thin regions become segments.
std::vector<Segment> detectLines(const Image& img, bool linear) {
    std::vector<Segment> out;
    if (img.w < 8 || img.h < 8) return out;
    // A copy at most kEdge pixels across: the preview and the export find the same lines, and
    // fine texture (bricks, leaves) doesn't break long edges up.
    constexpr int kEdge = 800;
    const int step = std::max(1, (std::max(img.w, img.h) + kEdge - 1) / kEdge);
    const int w = img.w / step, h = img.h / step;
    if (w < 8 || h < 8) return out;
    std::vector<float> lum(size_t(w) * h);
    parallelFor(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            double s = 0;
            for (int j = 0; j < step; ++j)
                for (int i = 0; i < step; ++i) {
                    const float* p = img.pixel(size_t(y * step + j) * img.w + x * step + i);
                    float Y = luminance(p[0], p[1], p[2]);
                    if (!std::isfinite(Y)) Y = 0.0f;
                    Y = std::clamp(Y, 0.0f, 1.0f);
                    s += linear ? colormath::linearToSrgb(Y) : Y;
                }
            lum[size_t(y) * w + x] = float(s / (step * step));
        }
    });
    // A light blur ([1 2 1] both ways), so noise and stair-stepped edges keep one direction.
    {
        std::vector<float> tmp(lum.size());
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const float* r = &lum[size_t(y) * w];
                tmp[size_t(y) * w + x] = 0.25f * (r[std::max(x - 1, 0)] + 2 * r[x] + r[std::min(x + 1, w - 1)]);
            }
        });
        parallelFor(h, [&](int y) {
            const float* a = &tmp[size_t(std::max(y - 1, 0)) * w];
            const float* b = &tmp[size_t(y) * w];
            const float* c = &tmp[size_t(std::min(y + 1, h - 1)) * w];
            for (int x = 0; x < w; ++x) lum[size_t(y) * w + x] = 0.25f * (a[x] + 2 * b[x] + c[x]);
        });
    }
    // Gradient (Sobel, per pixel) and its direction.
    std::vector<float> mag(lum.size(), 0.0f), ang(lum.size(), 0.0f);
    parallelFor(h, [&](int y) {
        if (y == 0 || y == h - 1) return;
        for (int x = 1; x < w - 1; ++x) {
            const auto L = [&](int i, int j) { return lum[size_t(y + j) * w + x + i]; };
            const float gx = (L(1, -1) + 2 * L(1, 0) + L(1, 1) - L(-1, -1) - 2 * L(-1, 0) - L(-1, 1)) / 8.0f;
            const float gy = (L(-1, 1) + 2 * L(0, 1) + L(1, 1) - L(-1, -1) - 2 * L(0, -1) - L(1, -1)) / 8.0f;
            mag[size_t(y) * w + x] = std::hypot(gx, gy);
            ang[size_t(y) * w + x] = std::atan2(gy, gx);
        }
    });
    constexpr float kThreshold = 0.02f;
    std::vector<int> seeds;
    for (int i = 0; i < int(mag.size()); ++i)
        if (mag[size_t(i)] > kThreshold) seeds.push_back(i);
    // Strongest first, as LSD does: segments grow from their clearest pixels. Ties by position, so
    // the order (and the result) doesn't depend on the sort.
    std::sort(seeds.begin(), seeds.end(), [&](int a, int b) { return mag[size_t(a)] != mag[size_t(b)] ? mag[size_t(a)] > mag[size_t(b)] : a < b; });
    std::vector<char> used(mag.size(), 0);
    const float minLen = 0.06f * float(std::max(w, h));
    constexpr float kTol = 22.5f * 3.14159265f / 180.0f;
    std::vector<int> region, stack;
    for (int seed : seeds) {
        if (used[size_t(seed)]) continue;
        region.clear();
        stack.assign(1, seed);
        used[size_t(seed)] = 1;
        // The region's gradient direction. Opposite gradients stay apart (as in LSD), so the two
        // sides of a thin line are two edges rather than one wide blob.
        double sc = std::cos(ang[size_t(seed)]), ss = std::sin(ang[size_t(seed)]);
        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            region.push_back(p);
            const int px = p % w, py = p / w;
            const double mean = std::atan2(ss, sc);
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) {
                    const int qx = px + i, qy = py + j;
                    if (qx < 1 || qy < 1 || qx >= w - 1 || qy >= h - 1) continue;
                    const int q = qy * w + qx;
                    if (used[size_t(q)] || mag[size_t(q)] <= kThreshold) continue;
                    const double d = std::fabs(std::remainder(double(ang[size_t(q)]) - mean, 2 * 3.14159265358979));
                    if (d > kTol) continue;
                    used[size_t(q)] = 1;
                    stack.push_back(q);
                    sc += std::cos(ang[size_t(q)]), ss += std::sin(ang[size_t(q)]);
                }
        }
        if (float(region.size()) < minLen) continue;
        // The region's main axis (principal component) is the line.
        double mx = 0, my = 0;
        for (int p : region) mx += p % w, my += p / w;
        mx /= region.size(), my /= region.size();
        double cxx = 0, cyy = 0, cxy = 0;
        for (int p : region) {
            const double dx = p % w - mx, dy = p / w - my;
            cxx += dx * dx, cyy += dy * dy, cxy += dx * dy;
        }
        const double th = 0.5 * std::atan2(2 * cxy, cxx - cyy);
        const double ux = std::cos(th), uy = std::sin(th);
        double lo = 1e30, hi = -1e30, spread = 0;
        for (int p : region) {
            const double dx = p % w - mx, dy = p / w - my;
            const double t = dx * ux + dy * uy, n = -dx * uy + dy * ux;
            lo = std::min(lo, t), hi = std::max(hi, t);
            spread += n * n;
        }
        spread = std::sqrt(spread / region.size());
        if (hi - lo < minLen || spread > 1.5) continue;  // short, or a blob rather than a line
        const float sx = 1.0f / w, sy = 1.0f / h;
        out.push_back({float((mx + ux * lo + 0.5) * sx), float((my + uy * lo + 0.5) * sy), float((mx + ux * hi + 0.5) * sx),
                       float((my + uy * hi + 0.5) * sy)});
    }
    return out;
}

void autoUprightAngles(const std::vector<Segment>& segments, int w, int h, int mode, double ang[3]) {
    ang[0] = ang[1] = ang[2] = 0;
    w = std::max(w, 1), h = std::max(h, 1);
    const double cx = w * 0.5, cy = h * 0.5, f = std::hypot(cx, cy);
    // Lines within 20 degrees of vertical or horizontal; the others (roofs, diagonals) say nothing.
    constexpr double kNear = 20.0 * 3.14159265358979 / 180.0;
    std::vector<UprightLine> lines;
    for (const Segment& s : segments) {
        const double dx = (s.x1 - s.x0) * w, dy = (s.y1 - s.y0) * h, len = std::hypot(dx, dy);
        if (len < 1e-6) continue;
        const double a = std::atan2(std::fabs(dy), std::fabs(dx));  // 0 horizontal .. pi/2 vertical
        const bool vertical = a > 3.14159265358979 / 2 - kNear;
        if (!vertical && a > kNear) continue;
        // Longer lines count for more (their residual is weighted by the square root of length).
        lines.push_back({(s.x0 * w - cx) / f, (s.y0 * h - cy) / f, (s.x1 * w - cx) / f, (s.y1 * h - cy) / f, vertical,
                         std::sqrt(len / f)});
    }
    if (lines.empty()) return;
    std::sort(lines.begin(), lines.end(), [](const UprightLine& a, const UprightLine& b) { return a.weight > b.weight; });
    if (lines.size() > 80) lines.resize(80);
    const bool freeLevel[3] = {false, false, true}, freeVertical[3] = {true, false, true}, freeAll[3] = {true, true, true};
    const bool* free = mode == UprightLevel ? freeLevel : mode == UprightVertical ? freeVertical : freeAll;
    // Robust: lines that stay more than 3 degrees off once the rest agree (a sloping roof, a
    // leaning tree) are dropped, and the rest solved again.
    for (int round = 0; round < 3; ++round) {
        solveUpright(lines, free, ang);
        std::vector<double> r;
        uprightResiduals(lines, ang, r);
        std::vector<UprightLine> kept;
        for (size_t i = 0; i < lines.size(); ++i)
            if (std::fabs(r[i] / lines[i].weight) < std::sin(3.0 * 3.14159265358979 / 180.0)) kept.push_back(lines[i]);
        if (kept.size() == lines.size() || kept.empty()) break;
        lines = std::move(kept);
    }
    if (mode == UprightAuto) {
        // Auto keeps the result natural: the horizontal perspective (rarely what was wrong) at
        // half strength, and nothing beyond 20 degrees.
        ang[1] *= 0.5;
        for (int k = 0; k < 3; ++k) ang[k] = std::clamp(ang[k], -0.35, 0.35);
    }
}

}  // namespace perspective
