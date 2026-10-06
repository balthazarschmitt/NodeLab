// Content-aware Remove (Lightroom's Remove without Generative AI, Photoshop's Content-Aware Fill):
// the masked area is rebuilt from patches of the rest of the image, so texture continues across
// it. Wexler, Shechtman and Irani's "Space-Time Completion" on an image pyramid, with the nearest
// patches found by PatchMatch (Barnes et al.). Deterministic: the random search hashes its
// position instead of keeping a generator, so the result doesn't depend on threads.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "graph/NodeRegistry.h"
#include "nodes/ImageOps.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

constexpr int kPatchR = 3;  // 7 x 7 patches
constexpr int kMinLevelEdge = 24;

struct Plane4 {
    int w = 0, h = 0;
    std::vector<float> px;  // RGBA
    float* at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
    const float* at(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
};

struct Level {
    Plane4 img;              // the known pixels (perceptual values), the hole being filled
    std::vector<uint8_t> hole;
    std::vector<uint8_t> source;  // patch centres whose whole patch is known and inside
};

uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u ^ (c + 0x165667B1u) * 0xC2B2AE3Du;
    h ^= h >> 15, h *= 0x2C1B3C6Du, h ^= h >> 12, h *= 0x297A2D39u, h ^= h >> 15;
    return h;
}

Level downsample(const Level& l) {
    Level d;
    d.img.w = std::max(1, l.img.w / 2), d.img.h = std::max(1, l.img.h / 2);
    d.img.px.assign(size_t(d.img.w) * d.img.h * 4, 0.0f);
    d.hole.assign(size_t(d.img.w) * d.img.h, 0);
    parallelFor(d.img.h, [&](int y) {
        for (int x = 0; x < d.img.w; ++x) {
            // Known pixels average only known ones; a cell with any hole is a hole.
            float acc[4] = {0, 0, 0, 0};
            int n = 0;
            bool hole = false;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i) {
                    const int sx = std::min(2 * x + i, l.img.w - 1), sy = std::min(2 * y + j, l.img.h - 1);
                    if (l.hole[size_t(sy) * l.img.w + sx]) {
                        hole = true;
                        continue;
                    }
                    const float* p = l.img.at(sx, sy);
                    for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    ++n;
                }
            d.hole[size_t(y) * d.img.w + x] = hole;
            float* o = d.img.at(x, y);
            for (int c = 0; c < 4; ++c) o[c] = n ? acc[c] / n : 0.0f;
        }
    });
    return d;
}

// Patch centres that can be copied from: the whole 7 x 7 patch inside the image and known.
void findSources(Level& l) {
    const int w = l.img.w, h = l.img.h;
    l.source.assign(size_t(w) * h, 0);
    // Distance to the nearest hole along rows, then a column check, as a box dilation.
    std::vector<uint8_t> rowFree(size_t(w) * h, 0);
    parallelFor(h, [&](int y) {
        for (int x = kPatchR; x < w - kPatchR; ++x) {
            bool ok = true;
            for (int i = -kPatchR; i <= kPatchR && ok; ++i) ok = !l.hole[size_t(y) * w + x + i];
            rowFree[size_t(y) * w + x] = ok;
        }
    });
    parallelFor(h, [&](int y) {
        if (y < kPatchR || y >= h - kPatchR) return;
        for (int x = kPatchR; x < w - kPatchR; ++x) {
            bool ok = true;
            for (int j = -kPatchR; j <= kPatchR && ok; ++j) ok = rowFree[size_t(y + j) * w + x];
            l.source[size_t(y) * w + x] = ok;
        }
    });
}

// The first guess at the coarsest level: the hole filled from its edge inwards, each ring the
// average of its known neighbours ("onion peel").
void peelFill(Level& l) {
    const int w = l.img.w, h = l.img.h;
    std::vector<uint8_t> known(l.hole.size());
    for (size_t i = 0; i < known.size(); ++i) known[i] = !l.hole[i];
    bool any = true;
    while (any) {
        any = false;
        std::vector<uint8_t> next = known;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (known[size_t(y) * w + x]) continue;
                float acc[4] = {0, 0, 0, 0};
                int n = 0;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        const int xx = x + i, yy = y + j;
                        if (xx < 0 || yy < 0 || xx >= w || yy >= h || !known[size_t(yy) * w + xx]) continue;
                        const float* p = l.img.at(xx, yy);
                        for (int c = 0; c < 4; ++c) acc[c] += p[c];
                        ++n;
                    }
                if (!n) continue;
                float* o = l.img.at(x, y);
                for (int c = 0; c < 4; ++c) o[c] = acc[c] / n;
                next[size_t(y) * w + x] = 1;
                any = true;
            }
        known.swap(next);
    }
}

// One level's completion: target patches are those touching the hole; each finds its nearest
// source patch (PatchMatch), and every hole pixel becomes the average of what the patches over
// it say it should be. `nnf` holds each target's source centre (x, y), or -1.
struct Completion {
    Level& l;
    std::vector<int> targets;  // indices of target centres
    std::vector<int32_t> nnf;  // 2 per pixel
    std::vector<float> cost;
    std::vector<int> sourceList;
    int level;

    Completion(Level& lv, int lvl) : l(lv), level(lvl) {
        const int w = l.img.w, h = l.img.h;
        nnf.assign(size_t(w) * h * 2, -1);
        cost.assign(size_t(w) * h, 1e30f);
        // Targets: within the patch radius of a hole pixel.
        std::vector<uint8_t> near(size_t(w) * h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                if (!l.hole[size_t(y) * w + x]) continue;
                for (int j = -kPatchR; j <= kPatchR; ++j)
                    for (int i = -kPatchR; i <= kPatchR; ++i) {
                        const int xx = x + i, yy = y + j;
                        if (xx >= 0 && yy >= 0 && xx < w && yy < h) near[size_t(yy) * w + xx] = 1;
                    }
            }
        for (int i = 0; i < w * h; ++i)
            if (near[size_t(i)]) targets.push_back(i);
        for (int i = 0; i < w * h; ++i)
            if (l.source[size_t(i)]) sourceList.push_back(i);
    }

    float distance(int tx, int ty, int sx, int sy, float limit) const {
        const int w = l.img.w, h = l.img.h;
        float d = 0;
        for (int j = -kPatchR; j <= kPatchR; ++j) {
            const int y = std::clamp(ty + j, 0, h - 1);
            for (int i = -kPatchR; i <= kPatchR; ++i) {
                const float* a = l.img.at(std::clamp(tx + i, 0, w - 1), y);
                const float* b = l.img.at(sx + i, sy + j);
                for (int c = 0; c < 4; ++c) d += (a[c] - b[c]) * (a[c] - b[c]);
            }
            if (d >= limit) return d;
        }
        return d;
    }

    void tryCandidate(int t, int sx, int sy) {
        const int w = l.img.w, h = l.img.h;
        if (sx < 0 || sy < 0 || sx >= w || sy >= h || !l.source[size_t(sy) * w + sx]) return;
        const int tx = t % w, ty = t / w;
        if (sx == tx && sy == ty) return;
        const float d = distance(tx, ty, sx, sy, cost[size_t(t)]);
        if (d < cost[size_t(t)]) cost[size_t(t)] = d, nnf[size_t(t) * 2] = sx, nnf[size_t(t) * 2 + 1] = sy;
    }

    // Initial matches: from the coarser level's (doubled) where given, else random.
    void init(const Completion* coarse) {
        const int w = l.img.w;
        parallelFor(int(targets.size()), [&](int k) {
            const int t = targets[size_t(k)], tx = t % w, ty = t / w;
            cost[size_t(t)] = 1e30f;
            if (coarse) {
                const int cw = coarse->l.img.w, ch = coarse->l.img.h;
                const int ct = std::min(ty / 2, ch - 1) * cw + std::min(tx / 2, cw - 1);
                const int csx = coarse->nnf[size_t(ct) * 2], csy = coarse->nnf[size_t(ct) * 2 + 1];
                if (csx >= 0) tryCandidate(t, csx * 2 + tx % 2, csy * 2 + ty % 2);
            }
            if (nnf[size_t(t) * 2] < 0 && !sourceList.empty()) {
                const int s = sourceList[hash3(uint32_t(t), uint32_t(level), 7u) % sourceList.size()];
                tryCandidate(t, s % w, s / w);
            }
        });
    }

    void rescore() {
        const int w = l.img.w;
        parallelFor(int(targets.size()), [&](int k) {
            const int t = targets[size_t(k)];
            const int sx = nnf[size_t(t) * 2], sy = nnf[size_t(t) * 2 + 1];
            cost[size_t(t)] = sx >= 0 ? distance(t % w, t / w, sx, sy, 1e30f) : 1e30f;
        });
    }

    // PatchMatch: neighbours' matches shifted by one (both scan directions, alternating), then a
    // random search around the current match at halving radii.
    void search(int iter) {
        const int w = l.img.w, h = l.img.h;
        const bool fwd = iter % 2 == 0;
        // Rows are independent in each sweep's vertical step only when processed in order, so
        // the propagation runs row by row: sequential within a row, rows in order.
        for (size_t kk = 0; kk < targets.size(); ++kk) {
            const int t = targets[fwd ? kk : targets.size() - 1 - kk];
            const int tx = t % w, ty = t / w, dir = fwd ? -1 : 1;
            for (const auto& nb : {std::pair{dir, 0}, std::pair{0, dir}}) {
                const int nx = tx + nb.first, ny = ty + nb.second;
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const int n = ny * w + nx;
                const int sx = nnf[size_t(n) * 2], sy = nnf[size_t(n) * 2 + 1];
                if (sx >= 0) tryCandidate(t, sx - nb.first, sy - nb.second);
            }
        }
        parallelFor(int(targets.size()), [&](int k) {
            const int t = targets[size_t(k)];
            int radius = std::max(w, h);
            uint32_t r = 0;
            while (radius >= 1) {
                const int sx = nnf[size_t(t) * 2], sy = nnf[size_t(t) * 2 + 1];
                if (sx < 0) break;
                const uint32_t hsh = hash3(uint32_t(t), uint32_t(iter * 64 + level), r++);
                const int dx = int(hsh % uint32_t(2 * radius + 1)) - radius;
                const int dy = int((hsh >> 16) % uint32_t(2 * radius + 1)) - radius;
                tryCandidate(t, sx + dx, sy + dy);
                radius /= 2;
            }
        });
    }

    // Each hole pixel: the average of the source pixels the patches over it map it to, weighted
    // towards close matches.
    void vote() {
        const int w = l.img.w, h = l.img.h;
        std::vector<float> accum(size_t(w) * h * 5, 0.0f);
        // Gathered per hole pixel (no write races): look at the targets within the patch radius.
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const size_t p = size_t(y) * w + x;
                if (!l.hole[p]) continue;
                float acc[4] = {0, 0, 0, 0}, wsum = 0;
                for (int j = -kPatchR; j <= kPatchR; ++j)
                    for (int i = -kPatchR; i <= kPatchR; ++i) {
                        const int tx = x - i, ty = y - j;
                        if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
                        const size_t t = size_t(ty) * w + tx;
                        const int sx = nnf[t * 2], sy = nnf[t * 2 + 1];
                        if (sx < 0) continue;
                        const float wt = 1.0f / (1.0f + cost[t] * 4.0f);
                        const float* s = l.img.at(sx + i, sy + j);
                        for (int c = 0; c < 4; ++c) acc[c] += wt * s[c];
                        wsum += wt;
                    }
                if (wsum <= 0.0f) continue;
                float* o = &accum[p * 5];
                for (int c = 0; c < 4; ++c) o[c] = acc[c] / wsum;
                o[4] = 1.0f;
            }
        });
        for (size_t p = 0; p < size_t(w) * h; ++p)
            if (accum[p * 5 + 4] > 0.0f) std::copy(&accum[p * 5], &accum[p * 5] + 4, &l.img.px[p * 4]);
    }
};

// Fills the hole of `full` (perceptual values) in place.
void complete(Plane4& full, const std::vector<uint8_t>& hole) {
    std::vector<Level> levels(1);
    levels[0].img = full;
    levels[0].hole = hole;
    while (std::min(levels.back().img.w, levels.back().img.h) / 2 >= kMinLevelEdge && levels.size() < 10)
        levels.push_back(downsample(levels.back()));
    for (Level& l : levels) findSources(l);
    // Coarsest level that still has places to copy from.
    int top = int(levels.size()) - 1;
    while (top > 0 && std::none_of(levels[size_t(top)].source.begin(), levels[size_t(top)].source.end(), [](uint8_t v) { return v; }))
        --top;
    if (std::none_of(levels[size_t(top)].source.begin(), levels[size_t(top)].source.end(), [](uint8_t v) { return v; })) {
        peelFill(levels[0]);  // nothing to copy from: a smooth fill
        full = levels[0].img;
        return;
    }
    peelFill(levels[size_t(top)]);
    std::unique_ptr<Completion> prev;
    for (int li = top; li >= 0; --li) {
        Level& l = levels[size_t(li)];
        if (prev) {
            // The coarser fill, upsampled, is the starting guess for the hole.
            const Plane4& c = prev->l.img;
            parallelFor(l.img.h, [&](int y) {
                for (int x = 0; x < l.img.w; ++x) {
                    if (!l.hole[size_t(y) * l.img.w + x]) continue;
                    const float* s = c.at(std::min(x / 2, c.w - 1), std::min(y / 2, c.h - 1));
                    std::copy(s, s + 4, l.img.at(x, y));
                }
            });
        }
        auto cur = std::make_unique<Completion>(l, li);
        cur->init(prev.get());
        // More passes where the image is small (cheap and where the structure is decided).
        const int passes = li == top ? 8 : li == 0 && levels.size() > 3 ? 2 : 4;
        for (int it = 0; it < passes; ++it) {
            for (int s = 0; s < 2; ++s) cur->search(it * 2 + s);
            cur->vote();
            cur->rescore();
        }
        prev = std::move(cur);
    }
    full = levels[0].img;
}

class RemoveNode : public Node {
public:
    NODELAB_NODE({"filter.remove", "Remove", "Filter",
                  {{"Image", PinType::Image}, {"Mask", PinType::Channel}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Grow", 2.0f, 0.0f, 50.0f)}})

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr maskCh = toChannel(in[1]);
        if (!maskCh || maskCh->constant) {
            // No mask (or a constant one): nothing to remove, or everything (which has nothing
            // to copy from).
            out[0] = Value(src);
            return;
        }
        const bool lin = ctx.linear();
        const int w = src->w, h = src->h;
        const ChannelSampler mask{maskCh.get(), w, h, 0.0f, 1.0f};
        // The hole: the mask above a trace, grown by Grow pixels (full resolution) so the edge of
        // the object goes too.
        std::vector<uint8_t> hole(size_t(w) * h, 0);
        bool any = false;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const bool m = mask(x, y) > 0.02f;
                hole[size_t(y) * w + x] = m;
                any |= m;
            }
        if (!any) {
            out[0] = Value(src);
            return;
        }
        const int grow = int(std::lround(paramF(0) * ctx.scale));
        if (grow > 0) {
            // Distance to the hole: pixels within `grow` join it.
            const std::vector<float> dist = imageops::distanceTransform(hole, w, h);
            for (size_t i = 0; i < hole.size(); ++i) hole[i] = dist[i] <= float(grow);
        }
        Plane4 work;
        work.w = w, work.h = h;
        work.px.resize(size_t(w) * h * 4);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const float* s = src->pixel(size_t(y) * w + x);
                float* d = work.at(x, y);
                for (int c = 0; c < 3; ++c) {
                    const float v = std::isfinite(s[c]) ? s[c] : 0.0f;
                    // Compared perceptually: in linear values the shadows' differences vanish.
                    d[c] = lin ? colormath::linearToSrgb(std::max(v, 0.0f)) : v;
                }
                d[3] = std::isfinite(s[3]) ? s[3] : 0.0f;
            }
        });
        complete(work, hole);
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                const float* s = src->pixel(i);
                float* d = img->pixel(i);
                if (!hole[i]) {
                    std::copy(s, s + 4, d);
                    continue;
                }
                // A soft mask edge blends the fill over the original.
                const float m = grow > 0 ? 1.0f : clamp01(mask(x, y) * 2.0f);
                const float* f = work.at(x, y);
                for (int c = 0; c < 4; ++c) {
                    float v = f[c];
                    if (c < 3 && lin) v = colormath::srgbToLinear(v);
                    d[c] = s[c] + (v - s[c]) * m;
                    if (!std::isfinite(d[c])) d[c] = v;
                }
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

}  // namespace

void registerRemoveNodes(NodeRegistry& r) { r.add<RemoveNode>(); }
