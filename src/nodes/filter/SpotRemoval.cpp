#include "nodes/filter/SpotRemoval.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/Parallel.h"
#include "graph/NodeRegistry.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

constexpr float kPi = 3.14159265f;
constexpr int kRing = 64;          // samples along the edge for Heal's membrane
constexpr float kLogEps = 1e-3f;   // linear Heal works on log(v + eps), so black stays finite

// Bilinear RGB with coordinates clamped to the buffer (pixel centres at +0.5).
void sampleRgb(const Image& img, float x, float y, float out[3]) {
    x = std::clamp(x - 0.5f, 0.0f, float(img.w - 1));
    y = std::clamp(y - 0.5f, 0.0f, float(img.h - 1));
    const int x0 = std::min(int(x), img.w - 1), y0 = std::min(int(y), img.h - 1);
    const int x1 = std::min(x0 + 1, img.w - 1), y1 = std::min(y0 + 1, img.h - 1);
    const float fx = x - x0, fy = y - y0;
    const float* a = img.pixel(size_t(y0) * img.w + x0);
    const float* b = img.pixel(size_t(y0) * img.w + x1);
    const float* c = img.pixel(size_t(y1) * img.w + x0);
    const float* d = img.pixel(size_t(y1) * img.w + x1);
    for (int k = 0; k < 3; ++k)
        out[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
}

// The average around a point (a small cross), so one noisy pixel on the edge doesn't tint the heal.
void sampleSoft(const Image& img, float x, float y, float r, float out[3]) {
    static const float kOff[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    out[0] = out[1] = out[2] = 0;
    for (const auto& o : kOff) {
        float s[3];
        sampleRgb(img, x + o[0] * r, y + o[1] * r, s);
        for (int k = 0; k < 3; ++k) out[k] += s[k] * 0.2f;
    }
}

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

float toWork(float v, bool linear) { return linear ? std::log(std::max(v, 0.0f) + kLogEps) : v; }
float fromWork(float v, bool linear) { return linear ? std::exp(v) - kLogEps : v; }

void applySpot(Image& img, const Spot& s, bool linear, int x0, int y0, int fullW, int fullH) {
    const float r = s.radius * float(std::max(fullW, fullH));
    if (!(r >= 0.5f) || !(s.opacity > 0.0f)) return;
    const float cx = s.x * fullW - x0, cy = s.y * fullH - y0;
    const float ox = (s.sx - s.x) * fullW, oy = (s.sy - s.y) * fullH;
    const int bx0 = std::max(0, int(std::floor(cx - r))), bx1 = std::min(img.w, int(std::ceil(cx + r)) + 1);
    const int by0 = std::max(0, int(std::floor(cy - r))), by1 = std::min(img.h, int(std::ceil(cy + r)) + 1);
    if (bx0 >= bx1 || by0 >= by1) return;

    // Heal: the target-minus-source difference along the edge (in the working form), filled in
    // smoothly across the inside, so the source's texture takes on the target's surroundings.
    float ringX[kRing], ringY[kRing], diff[kRing][3];
    if (s.heal) {
        const float soft = std::max(1.0f, r * 0.04f);
        for (int k = 0; k < kRing; ++k) {
            const float a = 2 * kPi * k / kRing;
            ringX[k] = cx + r * std::cos(a), ringY[k] = cy + r * std::sin(a);
            float t[3], src[3];
            sampleSoft(img, ringX[k], ringY[k], soft, t);
            sampleSoft(img, ringX[k] + ox, ringY[k] + oy, soft, src);
            for (int c = 0; c < 3; ++c) diff[k][c] = toWork(t[c], linear) - toWork(src[c], linear);
        }
    }

    // Results go to a scratch block first: later rows read sources this spot may overwrite.
    const int bw = bx1 - bx0, bh = by1 - by0;
    std::vector<float> block(size_t(bw) * bh * 3);
    const float inner = r * (1.0f - std::clamp(s.feather, 0.0f, 1.0f));
    const float minD2 = 1e-4f * r * r;
    parallelFor(bh, [&](int j) {
        const int y = by0 + j;
        for (int x = bx0; x < bx1; ++x) {
            float* o = &block[(size_t(j) * bw + (x - bx0)) * 3];
            const float* d = img.pixel(size_t(y) * img.w + x);
            const float px = x + 0.5f, py = y + 0.5f;
            const float dist = std::hypot(px - cx, py - cy);
            float a = dist <= inner ? 1.0f : dist >= r ? 0.0f : 1.0f - smoothstep(inner, r, dist);
            a *= std::clamp(s.opacity, 0.0f, 1.0f);
            if (a <= 0.0f) {
                std::copy(d, d + 3, o);
                continue;
            }
            float src[3];
            sampleRgb(img, px + ox, py + oy, src);
            if (s.heal) {
                // Inverse-distance (Shepard) weights over the edge samples: exact on the edge,
                // smooth inside.
                float acc[3] = {0, 0, 0}, wsum = 0;
                for (int k = 0; k < kRing; ++k) {
                    const float dx = px - ringX[k], dy = py - ringY[k];
                    const float w = 1.0f / (dx * dx + dy * dy + minD2);
                    wsum += w;
                    for (int c = 0; c < 3; ++c) acc[c] += w * diff[k][c];
                }
                for (int c = 0; c < 3; ++c) src[c] = fromWork(toWork(src[c], linear) + acc[c] / wsum, linear);
            }
            for (int c = 0; c < 3; ++c) o[c] = clampColor(linear, d[c] + (src[c] - d[c]) * a);
        }
    });
    for (int j = 0; j < bh; ++j)
        for (int x = bx0; x < bx1; ++x) {
            float* d = img.pixel(size_t(by0 + j) * img.w + x);
            const float* o = &block[(size_t(j) * bw + (x - bx0)) * 3];
            std::copy(o, o + 3, d);
        }
}

}  // namespace

void removeSpots(Image& img, const std::vector<Spot>& spots, bool linear, int x0, int y0, int fullW, int fullH) {
    if (img.empty()) return;
    if (fullW <= 0 || fullH <= 0) fullW = img.w, fullH = img.h;
    // In order, so a later spot can take its source from an earlier one's result, as in Lightroom.
    for (const Spot& s : spots) applySpot(img, s, linear, x0, y0, fullW, fullH);
}

void SpotRemovalNode::evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) {
    ImagePtr src = toImage(in[0], 0, 0);
    if (!src) return;
    if (spots.empty()) {
        out[0] = Value(src);
        return;
    }
    auto img = std::make_shared<Image>(*src);
    const PixelFrame fr = frameOf(ctx, img->w, img->h);
    removeSpots(*img, spots, ctx.linear(), fr.x0, fr.y0, fr.fullW, fr.fullH);
    out[0] = Value(ImagePtr(img));
}

void SpotRemovalNode::saveExtra(nlohmann::json& j) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const Spot& s : spots)
        arr.push_back({{"x", s.x}, {"y", s.y}, {"sx", s.sx}, {"sy", s.sy}, {"radius", s.radius},
                       {"feather", s.feather}, {"opacity", s.opacity}, {"mode", s.heal ? "heal" : "clone"}});
    j["spots"] = arr;
}

void SpotRemovalNode::loadExtra(const nlohmann::json& j) {
    spots.clear();
    active = -1;
    auto it = j.find("spots");
    if (it == j.end() || !it->is_array()) return;
    // Damaged values fall back to defaults and are clamped, like params.
    const auto num = [](const nlohmann::json& o, const char* k, float def, float lo, float hi) {
        auto f = o.find(k);
        if (f == o.end() || !f->is_number()) return def;
        const float v = f->get<float>();
        return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
    };
    for (const auto& o : *it) {
        if (!o.is_object()) continue;
        Spot s;
        s.x = num(o, "x", 0.5f, -1.0f, 2.0f), s.y = num(o, "y", 0.5f, -1.0f, 2.0f);
        s.sx = num(o, "sx", s.x, -1.0f, 2.0f), s.sy = num(o, "sy", s.y, -1.0f, 2.0f);
        s.radius = num(o, "radius", 0.03f, 0.002f, 0.3f);  // Size's range
        s.feather = num(o, "feather", 0.5f, 0.0f, 1.0f);
        s.opacity = num(o, "opacity", 1.0f, 0.0f, 1.0f);
        s.heal = !(o.contains("mode") && o["mode"] == "clone");
        spots.push_back(s);
    }
}

std::string SpotRemovalNode::signatureExtra() const {
    std::string sig = "spots:";
    char buf[160];
    for (const Spot& s : spots) {
        std::snprintf(buf, sizeof buf, "%a,%a,%a,%a,%a,%a,%a,%d;", s.x, s.y, s.sx, s.sy, s.radius, s.feather, s.opacity,
                      int(s.heal));
        sig += buf;
    }
    return sig;
}

int SpotRemovalNode::addSpot(float u, float v, float aspect) {
    Spot s;
    s.x = u, s.y = v;
    s.heal = paramI(0) == 0;
    s.radius = paramF(1), s.feather = paramF(2), s.opacity = paramF(3);
    // The radius in each axis's 0..1 units (the radius is relative to the long edge).
    aspect = std::max(aspect, 1e-3f);
    const float ru = aspect >= 1 ? s.radius : s.radius / aspect, rv = aspect >= 1 ? s.radius * aspect : s.radius;
    // A source just beside it, toward the middle of the image so it lands inside; the user drags
    // it onto clean texture.
    const float gap = 2.4f;
    s.sx = u + (u < 0.5f ? gap : -gap) * ru, s.sy = v;
    if (s.sx - ru < 0.0f || s.sx + ru > 1.0f) s.sx = u, s.sy = v + (v < 0.5f ? gap : -gap) * rv;
    spots.push_back(s);
    active = int(spots.size()) - 1;
    return active;
}

void SpotRemovalNode::loadActive() {
    if (active < 0 || active >= int(spots.size())) return;
    const Spot& s = spots[active];
    params[0] = s.heal ? 0 : 1;
    params[1] = std::clamp(s.radius, info().params[1].hardMin, info().params[1].hardMax);
    params[2] = s.feather;
    params[3] = s.opacity;
}

bool SpotRemovalNode::storeActive() {
    if (active < 0 || active >= int(spots.size())) return false;
    Spot& s = spots[active];
    const Spot before = s;
    s.heal = paramI(0) == 0;
    s.radius = paramF(1), s.feather = paramF(2), s.opacity = paramF(3);
    return s.heal != before.heal || s.radius != before.radius || s.feather != before.feather ||
           s.opacity != before.opacity;
}

void registerSpotRemovalNode(NodeRegistry& r) { r.add<SpotRemovalNode>(); }
