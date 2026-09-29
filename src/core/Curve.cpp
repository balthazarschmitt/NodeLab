#include "core/Curve.h"

#include <algorithm>
#include <cmath>

#include "graph/Node.h"

CurvePoints identityCurve() { return {{0.0f, 0.0f}, {1.0f, 1.0f}}; }

CurvePoints curveFromJson(const nlohmann::json& j) {
    CurvePoints pts;
    if (j.is_array()) {
        for (const auto& p : j)
            if (p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number())
                pts.push_back({std::clamp(p[0].get<float>(), 0.0f, 1.0f), std::clamp(p[1].get<float>(), 0.0f, 1.0f)});
    }
    if (pts.empty()) return identityCurve();
    std::sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    return pts;
}

nlohmann::json curveToJson(const CurvePoints& pts) {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& p : pts) j.push_back({p[0], p[1]});
    return j;
}

bool isIdentityCurve(const CurvePoints& pts) {
    for (const auto& p : pts)
        if (std::fabs(p[0] - p[1]) > 1e-5f) return false;
    return !pts.empty() && pts.front()[0] <= 1e-5f && pts.back()[0] >= 1 - 1e-5f;
}

float evalCurve(const CurvePoints& pts, float x) {
    const size_t n = pts.size();
    if (n == 0) return x;
    if (n == 1 || x <= pts.front()[0]) return pts.front()[1];
    if (x >= pts.back()[0]) return pts.back()[1];

    // Secant slopes and Fritsch-Carlson tangents.
    std::vector<float> d(n - 1), m(n);
    for (size_t k = 0; k + 1 < n; ++k) {
        float dx = std::max(pts[k + 1][0] - pts[k][0], 1e-6f);
        d[k] = (pts[k + 1][1] - pts[k][1]) / dx;
    }
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t k = 1; k + 1 < n; ++k) m[k] = (d[k - 1] * d[k] <= 0) ? 0.0f : (d[k - 1] + d[k]) * 0.5f;
    for (size_t k = 0; k + 1 < n; ++k) {
        if (std::fabs(d[k]) < 1e-9f) {
            m[k] = m[k + 1] = 0.0f;
            continue;
        }
        float a = m[k] / d[k], b = m[k + 1] / d[k];
        float s = a * a + b * b;
        if (s > 9.0f) {
            float t = 3.0f / std::sqrt(s);
            m[k] = t * a * d[k];
            m[k + 1] = t * b * d[k];
        }
    }

    size_t k = 0;
    while (k + 2 < n && x > pts[k + 1][0]) ++k;
    float h = std::max(pts[k + 1][0] - pts[k][0], 1e-6f);
    float t = (x - pts[k][0]) / h, t2 = t * t, t3 = t2 * t;
    float y = (2 * t3 - 3 * t2 + 1) * pts[k][1] + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * pts[k + 1][1] +
              (t3 - t2) * h * m[k + 1];
    return std::clamp(y, 0.0f, 1.0f);
}

std::vector<float> curveLut(const CurvePoints& pts, int n) {
    std::vector<float> lut(n);
    for (int i = 0; i < n; ++i) lut[i] = evalCurve(pts, float(i) / (n - 1));
    return lut;
}

float lutLookup(const std::vector<float>& lut, float x) {
    x = std::clamp(x, 0.0f, 1.0f) * float(lut.size() - 1);
    size_t i = std::min(size_t(x), lut.size() - 2);
    float f = x - float(i);
    return lut[i] + (lut[i + 1] - lut[i]) * f;
}

nlohmann::json defaultCurves() {
    nlohmann::json id = curveToJson(identityCurve());
    return {{"master", id}, {"r", id}, {"g", id}, {"b", id}};
}

ParamDesc ParamDesc::Curve(std::string n) {
    ParamDesc d = make(std::move(n), ParamKind::Curve, 0, 0, 0, 0, defaultCurves());
    return d;
}
