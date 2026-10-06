#include "nodes/filter/SpotRemoval.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "graph/NodeRegistry.h"
#include "nodes/ImageOps.h"
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

bool findSpotSource(const Image& img, std::vector<Spot>& spots, int index, bool avoidCurrent) {
    if (img.empty() || index < 0 || index >= int(spots.size())) return false;
    Spot& s = spots[size_t(index)];
    const float W = float(img.w), H = float(img.h);
    const float r = std::max(s.radius * std::max(W, H), 1.0f);
    const float tx = s.x * W, ty = s.y * H;
    // Ring samples just outside the spot (what the patch has to blend with), and inside it (a
    // candidate's own contents, which mustn't hold another blemish).
    constexpr int kAngles = 24, kRings = 2, kInner = 9;
    const float ringR[kRings] = {1.2f, 1.5f};
    struct Off {
        float x, y;
    };
    std::vector<Off> ring, inner;
    for (int k = 0; k < kRings; ++k)
        for (int i = 0; i < kAngles; ++i) {
            const float a = 2 * kPi * (i + 0.5f * k) / kAngles;
            ring.push_back({ringR[k] * r * std::cos(a), ringR[k] * r * std::sin(a)});
        }
    inner.push_back({0, 0});
    for (int i = 0; i < kInner - 1; ++i) {
        const float a = 2 * kPi * i / (kInner - 1);
        inner.push_back({0.55f * r * std::cos(a), 0.55f * r * std::sin(a)});
    }
    const size_t n = ring.size();
    // Ring samples off the image would compare its clamped edge, so only samples inside the image
    // around both the target and the candidate count (spots near an edge still find a source).
    auto inside = [&](float x, float y) { return x >= 0 && y >= 0 && x < W && y < H; };
    // The target's ring, and how much it varies (the texture the source should have).
    std::vector<float> target(n * 3);
    std::vector<char> tIn(n);
    float tMean[3] = {0, 0, 0};
    int tCount = 0;
    for (size_t i = 0; i < n; ++i) {
        sampleRgb(img, tx + ring[i].x, ty + ring[i].y, &target[i * 3]);
        tIn[i] = inside(tx + ring[i].x, ty + ring[i].y);
        if (!tIn[i]) continue;
        ++tCount;
        for (int c = 0; c < 3; ++c) tMean[c] += target[i * 3 + c];
    }
    if (tCount == 0) return false;
    for (float& m : tMean) m /= float(tCount);
    float tVar = 0;
    for (size_t i = 0; i < n; ++i)
        if (tIn[i])
            for (int c = 0; c < 3; ++c) tVar += (target[i * 3 + c] - tMean[c]) * (target[i * 3 + c] - tMean[c]) / float(tCount);
    // Scores are relative to the target's own texture plus a floor, so flat skin and busy
    // foliage both find their match.
    const float scale = 1.0f / (tVar + 1e-4f);

    const float margin = r;  // the source itself stays inside the image
    if (W < 2 * margin || H < 2 * margin) return false;
    const float step = std::max(1.0f, r * 0.5f), reach = r * 12.0f;
    const float x0 = std::max(margin, tx - reach), x1 = std::min(W - margin, tx + reach);
    const float y0 = std::max(margin, ty - reach), y1 = std::min(H - margin, ty + reach);
    float bestScore = INFINITY, bx = 0, by = 0;
    const float curX = s.sx * W, curY = s.sy * H;
    std::vector<float> cand(n * 3);
    std::vector<char> used(n);
    for (float cy = y0; cy <= y1; cy += step)
        for (float cx = x0; cx <= x1; cx += step) {
            const float d = std::hypot(cx - tx, cy - ty);
            if (d < 2.2f * r) continue;  // the source mustn't overlap the spot
            if (avoidCurrent && std::hypot(cx - curX, cy - curY) < 2.0f * r) continue;
            // Nor take another spot's blemish.
            bool clear = true;
            for (int j = 0; j < int(spots.size()) && clear; ++j) {
                if (j == index) continue;
                const Spot& o = spots[size_t(j)];
                const float ro = o.radius * std::max(W, H);
                clear = std::hypot(cx - o.x * W, cy - o.y * H) >= r + ro;
            }
            if (!clear) continue;
            float cMean[3] = {0, 0, 0}, pMean[3] = {0, 0, 0};
            int count = 0;
            for (size_t i = 0; i < n; ++i) {
                used[i] = tIn[i] && inside(cx + ring[i].x, cy + ring[i].y);
                if (!used[i]) continue;
                ++count;
                sampleRgb(img, cx + ring[i].x, cy + ring[i].y, &cand[i * 3]);
                for (int c = 0; c < 3; ++c) cMean[c] += cand[i * 3 + c], pMean[c] += target[i * 3 + c];
            }
            if (count * 2 < tCount) continue;  // too little of the ring to judge by
            for (int c = 0; c < 3; ++c) cMean[c] /= float(count), pMean[c] /= float(count);
            // Heal fixes the colour and brightness itself: compare texture (around the means).
            // Clone copies as it is: compare the values.
            float err = 0;
            for (size_t i = 0; i < n; ++i)
                if (used[i])
                    for (int c = 0; c < 3; ++c) {
                        const float e = s.heal ? (cand[i * 3 + c] - cMean[c]) - (target[i * 3 + c] - pMean[c])
                                               : cand[i * 3 + c] - target[i * 3 + c];
                        err += e * e;
                    }
            err /= float(count);
            // A candidate whose inside differs from its ring holds an edge or a blemish.
            float in = 0;
            for (const Off& o : inner) {
                float v[3];
                sampleRgb(img, cx + o.x, cy + o.y, v);
                for (int c = 0; c < 3; ++c) in += (v[c] - cMean[c]) * (v[c] - cMean[c]);
            }
            in /= float(inner.size());
            // Nearer is better among equals (the light and grain match best close by).
            const float score = (err + std::max(0.0f, in - tVar)) * scale + 0.02f * d / r;
            if (score < bestScore) bestScore = score, bx = cx, by = cy;
        }
    if (!std::isfinite(bestScore)) return false;
    s.sx = bx / W, s.sy = by / H;
    return true;
}

std::vector<Spot> detectDust(const Image& img, bool linear, float sensitivity) {
    std::vector<Spot> found;
    const int w = img.w, h = img.h;
    if (w < 16 || h < 16) return found;
    const int longEdge = std::max(w, h);
    // Perceptual lightness, so a spot's contrast means the same in shadows and sky.
    std::vector<float> l(size_t(w) * h);
    parallelFor(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            const float* p = img.pixel(size_t(y) * w + x);
            float v = luminance(p[0], p[1], p[2]);
            if (!std::isfinite(v)) v = 0.0f;
            l[size_t(y) * w + x] = linear ? colormath::linearToSrgb(std::clamp(v, 0.0f, 1.0f)) : std::clamp(v, 0.0f, 1.0f);
        }
    });
    // Sensor dust is a soft dark disc a little darker than its surroundings: lightness lightly
    // smoothed (against noise) below the local background (a wider blur).
    const float maxR = std::max(3.0f, 0.012f * longEdge);  // the largest spot, in pixels
    std::vector<float> fine = l, bg = l;
    imageops::blurChannel(fine, w, h, 0.8f, 0.8f);
    imageops::blurChannel(bg, w, h, maxR * 1.5f, maxR * 1.5f);
    // Texture: how much the lightness varies around each place. Dust is only looked for where
    // the background is smooth (sky, walls, studio backdrops); in foliage everything is a spot.
    std::vector<float> tex(size_t(w) * h);
    for (size_t i = 0; i < tex.size(); ++i) tex[i] = std::abs(fine[i] - bg[i]);
    imageops::blurChannel(tex, w, h, maxR * 3.0f, maxR * 3.0f);
    // Sensitivity 0..100: the contrast a spot needs, from 4% down to 0.5% of the lightness range.
    const float need = 0.04f * std::pow(0.125f, std::clamp(sensitivity, 0.0f, 100.0f) / 100.0f);
    std::vector<uint8_t> cand(size_t(w) * h, 0);
    parallelFor(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            const float d = bg[i] - fine[i];
            cand[i] = d > need && d > 3.0f * tex[i];
        }
    });
    // Connected blobs of candidates, kept when round and of a dust spot's size.
    std::vector<int> label(size_t(w) * h, -1), stack;
    struct Blob {
        double sx = 0, sy = 0, wsum = 0, peak = 0;
        int n = 0, x0, y0, x1, y1;
        bool edge = false;
    };
    std::vector<Blob> blobs;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t s = size_t(y) * w + x;
            if (!cand[s] || label[s] >= 0) continue;
            Blob b;
            b.x0 = b.x1 = x, b.y0 = b.y1 = y;
            const int id = int(blobs.size());
            label[s] = id;
            stack.assign(1, int(s));
            while (!stack.empty()) {
                const int p = stack.back();
                stack.pop_back();
                const int px = p % w, py = p / w;
                const float d = bg[size_t(p)] - fine[size_t(p)];
                b.sx += double(px + 0.5f) * d, b.sy += double(py + 0.5f) * d, b.wsum += d;
                b.peak = std::max(b.peak, double(d));
                ++b.n;
                b.x0 = std::min(b.x0, px), b.x1 = std::max(b.x1, px), b.y0 = std::min(b.y0, py), b.y1 = std::max(b.y1, py);
                b.edge |= px == 0 || py == 0 || px == w - 1 || py == h - 1;
                for (int k = 0; k < 4; ++k) {
                    const int nx = px + (k == 0) - (k == 1), ny = py + (k == 2) - (k == 3);
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const size_t q = size_t(ny) * w + nx;
                    if (cand[q] && label[q] < 0) label[q] = id, stack.push_back(int(q));
                }
            }
            blobs.push_back(b);
        }
    struct Hit {
        float x, y, r, score;
    };
    std::vector<Hit> hits;
    for (const Blob& b : blobs) {
        const int bw = b.x1 - b.x0 + 1, bh = b.y1 - b.y0 + 1;
        const float r = std::sqrt(float(b.n) / kPi);
        // Too big (a dark object, not dust), a line (an edge or a wire), or cut by the frame.
        if (b.edge || r > maxR || std::max(bw, bh) > 2.5f * std::min(bw, bh) || float(b.n) < 0.5f * bw * bh) continue;
        hits.push_back({float(b.sx / b.wsum), float(b.sy / b.wsum), r, float(b.peak) * float(b.n)});
    }
    // The clearest spots first, and no more than Lightroom would sensibly show.
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (hits.size() > 60) hits.resize(60);
    for (const Hit& hit : hits) {
        Spot s;
        s.x = hit.x / w, s.y = hit.y / h;
        // The circle covers the spot's soft edge too.
        s.radius = std::clamp((hit.r * 1.8f + 2.0f) / longEdge, 0.003f, 0.3f);
        s.sx = s.x, s.sy = s.y;
        found.push_back(s);
    }
    return found;
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
