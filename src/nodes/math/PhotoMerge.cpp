// Lightroom's Photo Merge as nodes: HDR Merge (bracketed exposures into one scene-linear image)
// and Panorama Merge (overlapping photos stitched). Both take any number of images: a new empty
// pin appears when the last one is connected, as on Layer Stack, and the count is kept in "extra".
// Everything is found from the pictures themselves (alignment, exposure ratios, the overlaps),
// on images scaled to a fixed working size, so the preview and the export line up.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <queue>
#include <string>
#include <vector>

#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "graph/Graph.h"
#include "graph/NodeRegistry.h"
#include "nodes/ImageOps.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

constexpr int kMinImages = 2, kMaxImages = 32;

NodeInfo withImagePins(NodeInfo inf, int n) {
    inf.inputs.clear();
    for (int i = 0; i < n; ++i) inf.inputs.push_back({"Image " + std::to_string(i), PinType::Image});
    return inf;
}

// Stretches to w x h (bilinear), as wires of different sizes are.
ImagePtr stretch(const ImagePtr& img, int w, int h) {
    if (img->w == w && img->h == h) return img;
    auto out = std::make_shared<Image>(w, h);
    const float sx = float(img->w) / float(w), sy = float(img->h) / float(h);
    parallelFor(h, [&](int y) {
        for (int x = 0; x < w; ++x)
            imageops::sampleBilinear(*img, (float(x) + 0.5f) * sx, (float(y) + 0.5f) * sy, out->pixel(size_t(y) * size_t(w) + size_t(x)));
    });
    return out;
}

float luma(const float* p) { return std::max(0.0f, 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]); }

std::vector<float> lumaPlane(const Image& img) {
    std::vector<float> y(img.pixelCount());
    parallelFor(img.h, [&](int r) {
        for (int x = 0; x < img.w; ++x) {
            const size_t i = size_t(r) * size_t(img.w) + size_t(x);
            const float v = luma(img.pixel(i));
            y[i] = std::isfinite(v) ? v : 0.0f;
        }
    });
    return y;
}

// Shared by both merges: the per-instance pins.
class MergeNode : public Node {
public:
    const NodeInfo& info() const override { return info_; }
    int images() const { return int(info_.inputs.size()); }
    void setImages(int n) { info_ = withImagePins(info_, std::clamp(n, kMinImages, kMaxImages)); }
    void saveExtra(nlohmann::json& j) const override { j = {{"images", images()}}; }
    void loadExtra(const nlohmann::json& j) override {
        if (!j.is_object()) return;
        if (const auto v = j.find("images"); v != j.end() && v->is_number_integer()) setImages(v->get<int>());
    }
    void linksChanged(Graph& g) override {
        // Always an empty pin at the end to connect the next photo to.
        if (g.inputLink(id, images() - 1) && images() < kMaxImages) setImages(images() + 1);
    }
    // Alignment and stitching look at every pixel.
    int roiPadding(const EvalContext&) const override { return kRoiWhole; }

protected:
    NodeInfo info_;

    struct Inputs {
        std::vector<ImagePtr> linear;  // the connected images in linear light, in pin order
        std::vector<int> pins;
    };
    static Inputs gather(const EvalContext& ctx, const std::vector<Value>& in) {
        Inputs r;
        for (size_t i = 0; i < in.size(); ++i) {
            int w, h;
            if (!in[i].size(w, h)) continue;
            ImagePtr img = toImage(in[i], w, h);
            if (!img || img->w < 1 || img->h < 1) continue;
            if (!ctx.linear()) {
                auto lin = std::make_shared<Image>(*img);
                parallelFor(lin->h, [&](int y) {
                    for (int x = 0; x < lin->w; ++x) {
                        float* p = lin->pixel(size_t(y) * size_t(lin->w) + size_t(x));
                        for (int c = 0; c < 3; ++c) p[c] = colormath::srgbToLinear(p[c]);
                    }
                });
                img = lin;
            }
            r.linear.push_back(img);
            r.pins.push_back(int(i));
        }
        return r;
    }
    // Back to the project's encoding, with non-finite values made safe.
    static void finish(const EvalContext& ctx, Image& img) {
        const bool encode = !ctx.linear();
        parallelFor(img.h, [&](int y) {
            for (int x = 0; x < img.w; ++x) {
                float* p = img.pixel(size_t(y) * size_t(img.w) + size_t(x));
                for (int c = 0; c < 4; ++c) {
                    if (!std::isfinite(p[c])) p[c] = 0.0f;
                    if (c < 3) p[c] = std::max(p[c], 0.0f);
                }
                if (encode)
                    for (int c = 0; c < 3; ++c) p[c] = colormath::linearToSrgb(p[c]);
            }
        });
    }
};

// ---------------------------------------------------------------- HDR Merge

// Ward's median threshold bitmaps: a picture's pixels above its median, which look the same at
// any exposure, so brackets can be aligned by comparing them. Pixels near the median are noise
// and left out (the exclusion bitmap).
struct Mtb {
    int w = 0, h = 0;
    std::vector<uint8_t> above, use;
};

std::vector<Mtb> mtbPyramid(const std::vector<float>& lum, int w, int h) {
    // Perceptual grey levels: thresholds behave alike in shadows and highlights.
    std::vector<float> g(lum.size());
    for (size_t i = 0; i < g.size(); ++i) g[i] = 255.0f * std::pow(std::min(lum[i], 1.0f), 1.0f / 2.2f);
    std::vector<Mtb> levels;
    int lw = w, lh = h;
    while (true) {
        Mtb m;
        m.w = lw, m.h = lh;
        std::array<size_t, 256> hist{};
        for (float v : g) ++hist[size_t(std::clamp(int(v), 0, 255))];
        size_t acc = 0;
        int median = 0;
        for (; median < 255 && (acc += hist[size_t(median)]) < g.size() / 2; ++median) {}
        m.above.resize(g.size());
        m.use.resize(g.size());
        for (size_t i = 0; i < g.size(); ++i) {
            m.above[i] = g[i] > float(median);
            m.use[i] = std::abs(g[i] - float(median)) > 4.0f;
        }
        levels.push_back(std::move(m));
        if (std::max(lw, lh) < 64 || levels.size() >= 10 || std::min(lw, lh) < 8) break;
        // Half size, averaging 2 x 2.
        const int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
        std::vector<float> n(size_t(nw) * size_t(nh));
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                const size_t a = size_t(2 * y) * size_t(lw) + size_t(2 * x);
                n[size_t(y) * size_t(nw) + size_t(x)] = 0.25f * (g[a] + g[a + 1] + g[a + size_t(lw)] + g[a + size_t(lw) + 1]);
            }
        g = std::move(n);
        lw = nw, lh = nh;
    }
    return levels;
}

// The shift (dx, dy) such that img(x + dx, y + dy) matches ref(x, y), coarse to fine.
void alignMtb(const std::vector<Mtb>& ref, const std::vector<Mtb>& img, int& dx, int& dy) {
    dx = dy = 0;
    const size_t levels = std::min(ref.size(), img.size());
    for (size_t l = levels; l-- > 0;) {
        dx *= 2, dy *= 2;
        const Mtb &a = ref[l], &b = img[l];
        if (a.w != b.w || a.h != b.h) continue;
        std::array<int64_t, 9> cost{};
        parallelFor(9, [&](int k) {
            const int sx = dx + k % 3 - 1, sy = dy + k / 3 - 1;
            int64_t c = 0, n = 0;
            for (int y = std::max(0, -sy); y < std::min(a.h, a.h - sy); ++y) {
                const size_t ra = size_t(y) * size_t(a.w), rb = size_t(y + sy) * size_t(a.w);
                for (int x = std::max(0, -sx); x < std::min(a.w, a.w - sx); ++x) {
                    const size_t i = ra + size_t(x), j = rb + size_t(x + sx);
                    if (a.use[i] & b.use[j]) c += a.above[i] ^ b.above[j], ++n;
                }
            }
            // Per compared pixel, so a big shift isn't favoured for overlapping less.
            cost[size_t(k)] = n ? c * 1000000 / n : INT64_MAX;
        });
        const int best = int(std::min_element(cost.begin(), cost.end()) - cost.begin());
        // Ties keep the centre (k = 4): no move without evidence.
        const int k = cost[4] <= cost[size_t(best)] ? 4 : best;
        dx += k % 3 - 1, dy += k / 3 - 1;
    }
}

class HdrMergeNode : public MergeNode {
public:
    static constexpr const char* kType = "math.hdr_merge";
    enum { AutoAlign, Deghost };
    static const NodeInfo& staticInfo() {
        static const NodeInfo inf =
            withImagePins({kType, "HDR Merge", "Mix", {}, {{"Image", PinType::Image}},
                           {ParamDesc::Bool("Auto Align", true), ParamDesc::Enum("Deghost", 0, {"None", "Low", "Medium", "High"})}},
                          kMinImages);
        return inf;
    }
    HdrMergeNode() {
        info_ = staticInfo();
        initParams();
    }

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        Inputs src = gather(ctx, in);
        if (src.linear.empty()) return;
        if (src.linear.size() == 1) {
            out[0] = in[size_t(src.pins[0])];
            return;
        }
        const int w = src.linear[0]->w, h = src.linear[0]->h;
        const size_t n = src.linear.size(), count = size_t(w) * size_t(h);
        std::vector<ImagePtr>& imgs = src.linear;
        for (ImagePtr& im : imgs) im = stretch(im, w, h);
        std::vector<std::vector<float>> lum(n);
        for (size_t i = 0; i < n; ++i) lum[i] = lumaPlane(*imgs[i]);

        // The reference is the middle exposure (by mean brightness): the others are aligned to
        // it and scaled to its exposure, so the result is about as bright as the middle frame.
        std::vector<double> mean(n, 0.0);
        for (size_t i = 0; i < n; ++i) mean[i] = std::accumulate(lum[i].begin(), lum[i].end(), 0.0) / double(count);
        std::vector<size_t> order(n);
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return mean[a] < mean[b]; });
        const size_t ref = order[(n - 1) / 2];

        std::vector<int> sx(n, 0), sy(n, 0);
        if (paramB(AutoAlign)) {
            const std::vector<Mtb> refBits = mtbPyramid(lum[ref], w, h);
            for (size_t i = 0; i < n; ++i)
                if (i != ref) alignMtb(refBits, mtbPyramid(lum[i], w, h), sx[i], sy[i]);
        }
        auto at = [&](size_t i, int x, int y) {
            return size_t(std::clamp(y + sy[i], 0, h - 1)) * size_t(w) + size_t(std::clamp(x + sx[i], 0, w - 1));
        };

        // Exposure ratios from pixels well exposed in both frames (the median, so moving things
        // and noise don't matter); the scale brings each frame to the reference's exposure.
        std::vector<float> scale(n, 1.0f);
        const int step = std::max(1, int(std::sqrt(double(count) / 250000.0)));
        for (size_t i = 0; i < n; ++i) {
            if (i == ref) continue;
            std::vector<float> ratios;
            for (int y = 0; y < h; y += step)
                for (int x = 0; x < w; x += step) {
                    const float r = lum[ref][size_t(y) * size_t(w) + size_t(x)], v = lum[i][at(i, x, y)];
                    if (r > 0.01f && r < 0.8f && v > 0.01f && v < 0.8f) ratios.push_back(v / r);
                }
            float ratio;
            if (ratios.size() >= 50) {
                std::nth_element(ratios.begin(), ratios.begin() + std::ptrdiff_t(ratios.size() / 2), ratios.end());
                ratio = ratios[ratios.size() / 2];
            } else {
                ratio = mean[ref] > 1e-6 ? float(mean[i] / mean[ref]) : 1.0f;
            }
            scale[i] = ratio > 1e-6f && std::isfinite(ratio) ? 1.0f / ratio : 1.0f;
        }
        // The darkest frame (largest scale) keeps the highlights, the brightest the shadows.
        const size_t darkest = size_t(std::max_element(scale.begin(), scale.end()) - scale.begin());
        const size_t brightest = size_t(std::min_element(scale.begin(), scale.end()) - scale.begin());

        // Debevec's hat: frames count most where they are mid-grey and not at all where clipped.
        auto weight = [&](size_t i, const float* p) {
            const float v = std::max({p[0], p[1], p[2]});
            const float t = std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f);
            float wgt = 1.0f - std::pow(2.0f * t - 1.0f, 12.0f);
            if (i == darkest && t > 0.5f) wgt = std::max(wgt, 1e-3f);
            if (i == brightest && t <= 0.5f) wgt = std::max(wgt, 1e-3f);
            return std::max(wgt, 1e-6f);
        };

        // Deghost: where a frame disagrees with the reference by more than a threshold (in stops),
        // something moved; that frame is left out there. The masks are softened, so the switch
        // between frames has no hard edges.
        std::vector<std::vector<float>> ghost(n);
        const int deghost = std::clamp(paramI(Deghost), 0, 3);
        if (deghost > 0) {
            const float stops[] = {0.0f, 1.0f, 0.6f, 0.35f};
            const float thr = stops[deghost];
            const float sigma = std::max(1.0f, 0.004f * float(std::max(w, h)));
            for (size_t i = 0; i < n; ++i) {
                if (i == ref) continue;
                ghost[i].assign(count, 0.0f);
                parallelFor(h, [&](int y) {
                    for (int x = 0; x < w; ++x) {
                        const size_t k = size_t(y) * size_t(w) + size_t(x), j = at(i, x, y);
                        if (weight(ref, imgs[ref]->pixel(k)) < 0.2f || weight(i, imgs[i]->pixel(j)) < 0.05f) continue;
                        const float d = std::abs(std::log2((lum[i][j] * scale[i] + 1e-4f) / (lum[ref][k] + 1e-4f)));
                        ghost[i][k] = d > thr ? 1.0f : 0.0f;
                    }
                });
                imageops::blurChannel(ghost[i], w, h, sigma, sigma);
            }
        }

        auto res = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const size_t k = size_t(y) * size_t(w) + size_t(x);
                double acc[3] = {0, 0, 0}, total = 0;
                for (size_t i = 0; i < n; ++i) {
                    const float* p = imgs[i]->pixel(at(i, x, y));
                    float wgt = weight(i, p);
                    if (!ghost[i].empty()) wgt *= std::clamp(1.0f - 2.0f * ghost[i][k], 0.0f, 1.0f);
                    if (i == ref) wgt = std::max(wgt, 1e-6f);
                    for (int c = 0; c < 3; ++c) acc[c] += double(wgt) * double(scale[i]) * double(p[c]);
                    total += wgt;
                }
                float* d = res->pixel(k);
                for (int c = 0; c < 3; ++c) d[c] = total > 0 ? float(acc[c] / total) : 0.0f;
                d[3] = imgs[ref]->pixel(k)[3];
            }
        });
        finish(ctx, *res);
        out[0] = Value(ImagePtr(res));
    }
};

// ---------------------------------------------------------------- Panorama Merge

using Mat3 = std::array<double, 9>;

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[size_t(i * 3 + j)] += a[size_t(i * 3 + k)] * b[size_t(k * 3 + j)];
    return r;
}

bool invert(const Mat3& m, Mat3& r) {
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return false;
    const double k = 1.0 / det;
    r = {(m[4] * m[8] - m[5] * m[7]) * k, (m[2] * m[7] - m[1] * m[8]) * k, (m[1] * m[5] - m[2] * m[4]) * k,
         (m[5] * m[6] - m[3] * m[8]) * k, (m[0] * m[8] - m[2] * m[6]) * k, (m[2] * m[3] - m[0] * m[5]) * k,
         (m[3] * m[7] - m[4] * m[6]) * k, (m[1] * m[6] - m[0] * m[7]) * k, (m[0] * m[4] - m[1] * m[3]) * k};
    return true;
}

bool apply(const Mat3& m, double x, double y, double& ox, double& oy) {
    const double z = m[6] * x + m[7] * y + m[8];
    if (!(z > 1e-9)) return false;
    ox = (m[0] * x + m[1] * y + m[2]) / z;
    oy = (m[3] * x + m[4] * y + m[5]) / z;
    return std::isfinite(ox) && std::isfinite(oy);
}

// Solves the n x n system a x = b in place (Gaussian elimination with partial pivoting).
bool solve(std::vector<double>& a, std::vector<double>& b, int n) {
    for (int c = 0; c < n; ++c) {
        int p = c;
        for (int r = c + 1; r < n; ++r)
            if (std::abs(a[size_t(r * n + c)]) > std::abs(a[size_t(p * n + c)])) p = r;
        if (std::abs(a[size_t(p * n + c)]) < 1e-12) return false;
        if (p != c) {
            for (int k = 0; k < n; ++k) std::swap(a[size_t(c * n + k)], a[size_t(p * n + k)]);
            std::swap(b[size_t(c)], b[size_t(p)]);
        }
        for (int r = c + 1; r < n; ++r) {
            const double f = a[size_t(r * n + c)] / a[size_t(c * n + c)];
            for (int k = c; k < n; ++k) a[size_t(r * n + k)] -= f * a[size_t(c * n + k)];
            b[size_t(r)] -= f * b[size_t(c)];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = b[size_t(r)];
        for (int k = r + 1; k < n; ++k) s -= a[size_t(r * n + k)] * b[size_t(k)];
        b[size_t(r)] = s / a[size_t(r * n + r)];
    }
    return true;
}

struct Pt {
    double x, y;
};
struct Match {
    Pt a, b;
};

// A homography from point pairs by least squares (h33 = 1), a -> b. Points are in centred
// working pixels, a few hundred at most, so double precision needs no extra normalisation.
bool fitHomography(const std::vector<Match>& m, Mat3& H) {
    if (m.size() < 4) return false;
    std::vector<double> a(64, 0.0), b(8, 0.0);
    for (const Match& p : m) {
        const double rows[2][9] = {{p.a.x, p.a.y, 1, 0, 0, 0, -p.a.x * p.b.x, -p.a.y * p.b.x, p.b.x},
                                   {0, 0, 0, p.a.x, p.a.y, 1, -p.a.x * p.b.y, -p.a.y * p.b.y, p.b.y}};
        for (const auto& r : rows)
            for (int i = 0; i < 8; ++i) {
                for (int j = 0; j < 8; ++j) a[size_t(i * 8 + j)] += r[i] * r[j];
                b[size_t(i)] += r[i] * r[8];
            }
    }
    if (!solve(a, b, 8)) return false;
    H = {b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], 1.0};
    return std::all_of(H.begin(), H.end(), [](double v) { return std::isfinite(v); });
}

// A picture scaled to the working size, in perceptual grey, with its corner features.
struct Work {
    int w = 0, h = 0;
    float factor = 1.0f;  // input pixels per working pixel
    std::vector<float> grey, lum;  // grey: perceptual, for features; lum: linear, for exposure
    std::vector<Pt> pts;           // centred working coordinates
    std::vector<std::array<float, 64>> desc;
};

constexpr int kWorkEdge = 800, kDescRadius = 8;

Work makeWork(const Image& img) {
    Work wk;
    const int f = std::max(1, int(std::ceil(float(std::max(img.w, img.h)) / float(kWorkEdge))));
    wk.factor = float(f);
    wk.w = std::max(1, img.w / f), wk.h = std::max(1, img.h / f);
    wk.grey.assign(size_t(wk.w) * size_t(wk.h), 0.0f);
    wk.lum.assign(wk.grey.size(), 0.0f);
    parallelFor(wk.h, [&](int y) {
        for (int x = 0; x < wk.w; ++x) {
            double s = 0;
            int c = 0;
            for (int v = y * f; v < std::min(img.h, (y + 1) * f); ++v)
                for (int u = x * f; u < std::min(img.w, (x + 1) * f); ++u, ++c) {
                    const float l = luma(img.pixel(size_t(v) * size_t(img.w) + size_t(u)));
                    s += std::isfinite(l) ? l : 0.0f;
                }
            const float l = c ? float(s / c) : 0.0f;
            wk.lum[size_t(y) * size_t(wk.w) + size_t(x)] = l;
            wk.grey[size_t(y) * size_t(wk.w) + size_t(x)] = std::pow(std::min(l, 1.0f), 1.0f / 2.2f);
        }
    });
    if (wk.w < 4 * kDescRadius || wk.h < 4 * kDescRadius) return wk;

    // Harris corners, spread over the picture: the strongest few in each cell of a grid.
    std::vector<float> smooth = wk.grey;
    imageops::blurChannel(smooth, wk.w, wk.h, 1.0f, 1.0f);
    const size_t n = smooth.size();
    std::vector<float> xx(n, 0.0f), yy(n, 0.0f), xy(n, 0.0f);
    for (int y = 1; y < wk.h - 1; ++y)
        for (int x = 1; x < wk.w - 1; ++x) {
            const size_t i = size_t(y) * size_t(wk.w) + size_t(x);
            const float gx = 0.5f * (smooth[i + 1] - smooth[i - 1]), gy = 0.5f * (smooth[i + size_t(wk.w)] - smooth[i - size_t(wk.w)]);
            xx[i] = gx * gx, yy[i] = gy * gy, xy[i] = gx * gy;
        }
    imageops::blurChannel(xx, wk.w, wk.h, 1.5f, 1.5f);
    imageops::blurChannel(yy, wk.w, wk.h, 1.5f, 1.5f);
    imageops::blurChannel(xy, wk.w, wk.h, 1.5f, 1.5f);
    std::vector<float> R(n, 0.0f);
    float maxR = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        R[i] = xx[i] * yy[i] - xy[i] * xy[i] - 0.04f * (xx[i] + yy[i]) * (xx[i] + yy[i]);
        maxR = std::max(maxR, R[i]);
    }
    if (!(maxR > 0.0f)) return wk;
    constexpr int kCell = 20, kPerCell = 3;
    const int b = kDescRadius + 2;
    for (int cy = b; cy < wk.h - b; cy += kCell)
        for (int cx = b; cx < wk.w - b; cx += kCell) {
            std::vector<std::pair<float, size_t>> cand;
            for (int y = cy; y < std::min(cy + kCell, wk.h - b); ++y)
                for (int x = cx; x < std::min(cx + kCell, wk.w - b); ++x) {
                    const size_t i = size_t(y) * size_t(wk.w) + size_t(x);
                    const float r = R[i];
                    if (r < maxR * 1e-3f) continue;
                    bool peak = true;
                    for (int dy = -1; dy <= 1 && peak; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            if ((dx || dy) && R[size_t(y + dy) * size_t(wk.w) + size_t(x + dx)] > r) {
                                peak = false;
                                break;
                            }
                    if (peak) cand.push_back({r, i});
                }
            std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& c) { return a.first > c.first || (a.first == c.first && a.second < c.second); });
            if (cand.size() > size_t(kPerCell)) cand.resize(size_t(kPerCell));
            for (const auto& [r, i] : cand) {
                const int x = int(i % size_t(wk.w)), y = int(i / size_t(wk.w));
                // An 8 x 8 patch, every other pixel of the smoothed picture, normalised so
                // exposure and contrast differences don't matter.
                std::array<float, 64> d{};
                double mean = 0, sq = 0;
                for (int k = 0; k < 64; ++k) {
                    const int px = x + (k % 8) * 2 - 7, py = y + (k / 8) * 2 - 7;
                    d[size_t(k)] = smooth[size_t(py) * size_t(wk.w) + size_t(px)];
                    mean += d[size_t(k)];
                }
                mean /= 64;
                for (float& v : d) v -= float(mean), sq += double(v) * v;
                if (sq < 64 * 1e-6) continue;  // flat: matches anything
                const float inv = float(1.0 / std::sqrt(sq));
                for (float& v : d) v *= inv;
                wk.pts.push_back({x + 0.5 - wk.w * 0.5, y + 0.5 - wk.h * 0.5});
                wk.desc.push_back(d);
            }
        }
    return wk;
}

// Mutual nearest neighbours that pass Lowe's ratio test.
std::vector<Match> matchFeatures(const Work& a, const Work& b) {
    auto nearest = [](const Work& p, const Work& q, std::vector<int>& best) {
        best.assign(p.desc.size(), -1);
        parallelFor(int(p.desc.size()), [&](int i) {
            float d1 = 1e30f, d2 = 1e30f;
            int k1 = -1;
            for (size_t j = 0; j < q.desc.size(); ++j) {
                float d = 0;
                for (int k = 0; k < 64; ++k) {
                    const float e = p.desc[size_t(i)][size_t(k)] - q.desc[j][size_t(k)];
                    d += e * e;
                }
                if (d < d1) d2 = d1, d1 = d, k1 = int(j);
                else if (d < d2) d2 = d;
            }
            if (k1 >= 0 && d1 < 0.64f * d2) best[size_t(i)] = k1;
        });
    };
    std::vector<int> ab, ba;
    nearest(a, b, ab);
    nearest(b, a, ba);
    std::vector<Match> m;
    for (size_t i = 0; i < ab.size(); ++i)
        if (ab[i] >= 0 && ba[size_t(ab[i])] == int(i)) m.push_back({a.pts[i], b.pts[size_t(ab[i])]});
    return m;
}

// RANSAC with a fixed seed (the same result every run). Returns the inliers.
std::vector<Match> ransac(const std::vector<Match>& m, Mat3& H) {
    std::vector<Match> best;
    if (m.size() < 8) return best;
    uint32_t seed = 12345u;
    auto next = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    constexpr double kThreshold = 3.0;  // working pixels
    auto inliers = [&](const Mat3& h) {
        std::vector<Match> in;
        for (const Match& p : m) {
            double x, y;
            if (apply(h, p.a.x, p.a.y, x, y) && (x - p.b.x) * (x - p.b.x) + (y - p.b.y) * (y - p.b.y) < kThreshold * kThreshold)
                in.push_back(p);
        }
        return in;
    };
    for (int it = 0; it < 1000; ++it) {
        size_t s[4];
        for (int k = 0; k < 4; ++k) {
            s[k] = next() % m.size();
            for (int j = 0; j < k; ++j)
                if (s[j] == s[k]) s[k] = (s[k] + 1) % m.size(), j = -1;
        }
        Mat3 h;
        if (!fitHomography({m[s[0]], m[s[1]], m[s[2]], m[s[3]]}, h)) continue;
        std::vector<Match> in = inliers(h);
        if (in.size() > best.size()) best = std::move(in), H = h;
    }
    // Refit on every inlier, twice (the set can grow).
    for (int k = 0; k < 2 && best.size() >= 8; ++k) {
        Mat3 h;
        if (!fitHomography(best, h)) break;
        std::vector<Match> in = inliers(h);
        if (in.size() < best.size()) break;
        best = std::move(in), H = h;
    }
    return best;
}

// The focal lengths a rotation-only homography implies (Szeliski and Shum, as OpenCV's
// focalsFromHomography).
bool focalFromHomography(const Mat3& h, double& f) {
    auto one = [](double d1, double d2, double v1, double v2, double& out) {
        if (v1 < v2) std::swap(v1, v2);
        if (v1 > 0 && v2 > 0) out = std::sqrt(std::abs(d1) > std::abs(d2) ? v1 : v2);
        else if (v1 > 0) out = std::sqrt(v1);
        else return false;
        return std::isfinite(out);
    };
    double f0, f1;
    double d1 = h[6] * h[7], d2 = (h[7] - h[6]) * (h[7] + h[6]);
    if (!one(d1, d2, -(h[0] * h[1] + h[3] * h[4]) / d1, (h[0] * h[0] + h[3] * h[3] - h[1] * h[1] - h[4] * h[4]) / d2, f1)) return false;
    d1 = h[0] * h[3] + h[1] * h[4], d2 = h[0] * h[0] + h[1] * h[1] - h[3] * h[3] - h[4] * h[4];
    if (!one(d1, d2, -h[2] * h[5] / d1, (h[5] * h[5] - h[2] * h[2]) / d2, f0)) return false;
    f = std::sqrt(f0 * f1);
    return f > 0 && std::isfinite(f);
}

// The largest axis-aligned rectangle of set cells (the histogram method). Returns x, y, w, h.
std::array<int, 4> largestRectangle(const std::vector<uint8_t>& m, int w, int h) {
    std::array<int, 4> best{0, 0, 0, 0};
    std::vector<int> height(size_t(w), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) height[size_t(x)] = m[size_t(y) * size_t(w) + size_t(x)] ? height[size_t(x)] + 1 : 0;
        std::vector<int> stack;
        for (int x = 0; x <= w; ++x) {
            const int cur = x < w ? height[size_t(x)] : 0;
            while (!stack.empty() && height[size_t(stack.back())] >= cur) {
                const int top = height[size_t(stack.back())];
                stack.pop_back();
                const int left = stack.empty() ? 0 : stack.back() + 1;
                if (int64_t(top) * (x - left) > int64_t(best[2]) * best[3]) best = {left, y - top + 1, x - left, top};
            }
            stack.push_back(x);
        }
    }
    return best;
}

class PanoramaMergeNode : public MergeNode {
public:
    static constexpr const char* kType = "math.panorama_merge";
    enum { Projection, AutoCrop };
    enum { Spherical, Cylindrical, Perspective };
    static const NodeInfo& staticInfo() {
        static const NodeInfo inf = withImagePins(
            {kType, "Panorama Merge", "Mix", {}, {{"Image", PinType::Image}},
             {ParamDesc::Enum("Projection", Spherical, {"Spherical", "Cylindrical", "Perspective"}), ParamDesc::Bool("Auto Crop", false)}},
            kMinImages);
        return inf;
    }
    PanoramaMergeNode() {
        info_ = staticInfo();
        initParams();
    }

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        Inputs src = gather(ctx, in);
        if (src.linear.empty()) return;
        const std::vector<ImagePtr>& imgs = src.linear;
        const size_t n = imgs.size();
        std::vector<Work> work(n);
        for (size_t i = 0; i < n; ++i) work[i] = makeWork(*imgs[i]);

        // Every pair that overlaps: its homography (j -> i) and the matches behind it.
        struct Pair {
            bool ok = false;
            Mat3 H{};  // centred working coordinates of j -> those of i
            std::vector<Match> inliers;
        };
        std::vector<Pair> pairs(n * n);
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j) {
                const std::vector<Match> m = matchFeatures(work[j], work[i]);  // a in j, b in i
                Mat3 H;
                std::vector<Match> inl = ransac(m, H);
                // Brown and Lowe's test that the inliers aren't chance.
                if (inl.size() < 15 || double(inl.size()) < 8.0 + 0.3 * double(m.size())) continue;
                const double det = H[0] * H[4] - H[1] * H[3];
                if (!(det > 0.2 && det < 5.0)) continue;
                Mat3 inv;
                if (!invert(H, inv)) continue;
                Pair& p = pairs[i * n + j];
                p.ok = true, p.H = H, p.inliers = inl;
                Pair& q = pairs[j * n + i];
                q.ok = true, q.H = inv;
                for (const Match& mm : inl) q.inliers.push_back({mm.b, mm.a});
            }

        // A maximum spanning tree by inlier count, from the best-connected picture, gives each
        // picture's homography to the reference's frame.
        std::vector<size_t> degree(n, 0);
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n; ++j)
                if (pairs[i * n + j].ok) degree[i] += pairs[i * n + j].inliers.size();
        const size_t ref = size_t(std::max_element(degree.begin(), degree.end()) - degree.begin());
        std::vector<Mat3> toRef(n);
        std::vector<bool> placed(n, false);
        toRef[ref] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        placed[ref] = true;
        while (true) {
            size_t bi = 0, bj = 0, bestCount = 0;
            for (size_t i = 0; i < n; ++i)
                if (placed[i])
                    for (size_t j = 0; j < n; ++j)
                        if (!placed[j] && pairs[i * n + j].ok && pairs[i * n + j].inliers.size() > bestCount)
                            bi = i, bj = j, bestCount = pairs[i * n + j].inliers.size();
            if (!bestCount) break;
            toRef[bj] = mul(toRef[bi], pairs[bi * n + bj].H);
            placed[bj] = true;
        }
        std::vector<size_t> used;
        for (size_t i = 0; i < n; ++i)
            if (placed[i]) used.push_back(i);
        if (used.size() < 2) {
            // Nothing overlaps: the first picture, as it came in.
            out[0] = in[size_t(src.pins[0])];
            return;
        }

        // Focal length (working pixels) for the curved projections, from the homographies.
        std::vector<double> focals;
        for (size_t i = 0; i < n; ++i)
            for (size_t j = i + 1; j < n; ++j) {
                double f;
                if (pairs[i * n + j].ok && placed[i] && placed[j] && focalFromHomography(pairs[i * n + j].H, f)) focals.push_back(f);
            }
        const Work& rw = work[ref];
        double fWork = double(std::max(rw.w, rw.h));  // about 53 degrees across, if unknown
        if (!focals.empty()) {
            std::nth_element(focals.begin(), focals.begin() + std::ptrdiff_t(focals.size() / 2), focals.end());
            fWork = std::clamp(focals[focals.size() / 2], 0.3 * fWork, 20.0 * fWork);
        }

        // Into each picture's own pixels: centred input coordinates = working ones x factor.
        const double fr = rw.factor;
        const double f = fWork * fr;  // in the reference's input pixels
        std::vector<Mat3> fromOut(n);  // reference plane (centred input px) -> picture i (centred input px)
        for (size_t i : used) {
            const double fi = work[i].factor;
            const Mat3 S = {fr, 0, 0, 0, fr, 0, 0, 0, 1}, Si = {1.0 / fi, 0, 0, 0, 1.0 / fi, 0, 0, 0, 1};
            const Mat3 native = mul(S, mul(toRef[i], Si));  // picture i -> reference, input px
            if (!invert(native, fromOut[i])) fromOut[i] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        }

        const int proj = std::clamp(paramI(Projection), 0, 2);
        // Output (u, v), in reference pixels around its centre, to a point on the reference plane.
        auto toPlane = [&](double u, double v, double& px, double& py) {
            double X, Y, Z;
            if (proj == Perspective) {
                px = u, py = v;
                return true;
            }
            const double th = u / f;
            if (proj == Cylindrical) X = std::sin(th), Y = v / f, Z = std::cos(th);
            else {
                const double ph = v / f;
                X = std::cos(ph) * std::sin(th), Y = std::sin(ph), Z = std::cos(ph) * std::cos(th);
            }
            if (Z < 1e-3) return false;
            px = f * X / Z, py = f * Y / Z;
            return true;
        };
        auto fromPlane = [&](double px, double py, double& u, double& v) {
            if (proj == Perspective) {
                u = px, v = py;
                return;
            }
            const double X = px, Y = py, Z = f, r = std::sqrt(X * X + Z * Z);
            u = f * std::atan2(X, Z);
            v = proj == Cylindrical ? f * Y / r : f * std::atan2(Y, r);
        };

        // Bounds of every picture's outline in the output.
        const double refW = imgs[ref]->w, refH = imgs[ref]->h;
        const double limit = 6.0 * std::max(refW, refH) * double(used.size());
        double minU = 1e300, minV = 1e300, maxU = -1e300, maxV = -1e300;
        std::vector<std::array<double, 4>> box(n, {1e300, 1e300, -1e300, -1e300});
        for (size_t i : used) {
            const double W = imgs[i]->w, Hh = imgs[i]->h;
            Mat3 toPlaneH;
            if (!invert(fromOut[i], toPlaneH)) continue;
            for (int k = 0; k < 4 * 64; ++k) {
                const double t = (k % 64) / 64.0;
                const int side = k / 64;
                const double x = side == 0 ? t * W : side == 1 ? W : side == 2 ? (1 - t) * W : 0;
                const double y = side == 0 ? 0 : side == 1 ? t * Hh : side == 2 ? Hh : (1 - t) * Hh;
                double px, py, u, v;
                if (!apply(toPlaneH, x - W * 0.5, y - Hh * 0.5, px, py)) continue;
                fromPlane(px, py, u, v);
                u = std::clamp(u, -limit, limit), v = std::clamp(v, -limit, limit);
                auto& b = box[i];
                b = {std::min(b[0], u), std::min(b[1], v), std::max(b[2], u), std::max(b[3], v)};
            }
            minU = std::min(minU, box[i][0]), minV = std::min(minV, box[i][1]);
            maxU = std::max(maxU, box[i][2]), maxV = std::max(maxV, box[i][3]);
        }
        if (!(maxU > minU && maxV > minV)) {
            out[0] = in[size_t(src.pins[0])];
            return;
        }
        // Keep the output a sane size (a wide perspective panorama stretches without end).
        double sc = 1.0;
        const double maxEdge = 8.0 * std::max(refW, refH);
        sc = std::min(sc, maxEdge / std::max(maxU - minU, maxV - minV));
        int ow = std::max(1, int(std::ceil((maxU - minU) * sc))), oh = std::max(1, int(std::ceil((maxV - minV) * sc)));

        // Gain compensation (Brown and Lowe): one gain per picture so overlaps match in
        // brightness, pulled towards 1 so the whole doesn't drift.
        std::vector<double> gain(n, 1.0);
        {
            const int m = int(used.size());
            std::vector<int> slot(n, -1);
            for (int k = 0; k < m; ++k) slot[used[size_t(k)]] = k;
            std::vector<double> A(size_t(m * m), 0.0), B(size_t(m), 0.0);
            constexpr double sigmaN = 0.04, sigmaG = 0.1;
            auto patch = [](const Work& w, const Pt& p) {
                const int cx = int(p.x + w.w * 0.5), cy = int(p.y + w.h * 0.5);
                double s = 0;
                int c = 0;
                for (int y = std::max(0, cy - 2); y <= std::min(w.h - 1, cy + 2); ++y)
                    for (int x = std::max(0, cx - 2); x <= std::min(w.w - 1, cx + 2); ++x, ++c) s += w.lum[size_t(y) * size_t(w.w) + size_t(x)];
                return c ? s / c : 0.0;
            };
            for (size_t i : used)
                for (size_t j : used) {
                    const Pair& p = pairs[i * n + j];
                    if (i == j || !p.ok) continue;
                    // p maps j -> i: match.a is in j, match.b in i.
                    double Ii = 0, Ij = 0;
                    for (const Match& mm : p.inliers) Ii += patch(work[i], mm.b), Ij += patch(work[j], mm.a);
                    const double N = double(p.inliers.size());
                    Ii /= N, Ij /= N;
                    const int a = slot[i], b = slot[j];
                    A[size_t(a * m + a)] += N * (Ii * Ii / (sigmaN * sigmaN) + 1.0 / (sigmaG * sigmaG));
                    A[size_t(a * m + b)] -= N * Ii * Ij / (sigmaN * sigmaN);
                    B[size_t(a)] += N / (sigmaG * sigmaG);
                }
            if (solve(A, B, m))
                for (int k = 0; k < m; ++k)
                    if (std::isfinite(B[size_t(k)]) && B[size_t(k)] > 0.2 && B[size_t(k)] < 5.0) gain[used[size_t(k)]] = B[size_t(k)];
        }

        // Each output pixel: every picture that covers it, feathered towards its edges so seams
        // fade. The feather is steep, so mostly one picture shows (less ghosting).
        auto res = std::make_shared<Image>(ow, oh);
        parallelFor(oh, [&](int y) {
            for (int x = 0; x < ow; ++x) {
                const double u = minU + (x + 0.5) / sc, v = minV + (y + 0.5) / sc;
                double px, py;
                float* d = res->pixel(size_t(y) * size_t(ow) + size_t(x));
                if (!toPlane(u, v, px, py)) continue;
                double acc[4] = {0, 0, 0, 0}, total = 0;
                for (size_t i : used) {
                    const auto& b = box[i];
                    if (u < b[0] - 1 || u > b[2] + 1 || v < b[1] - 1 || v > b[3] + 1) continue;
                    const double W = imgs[i]->w, Hh = imgs[i]->h;
                    double sx, sy;
                    if (!apply(fromOut[i], px, py, sx, sy)) continue;
                    sx += W * 0.5, sy += Hh * 0.5;
                    if (sx < 0 || sy < 0 || sx > W || sy > Hh) continue;
                    const double wx = std::min(sx, W - sx) / (W * 0.5), wy = std::min(sy, Hh - sy) / (Hh * 0.5);
                    const double wgt = std::pow(std::max(wx * wy, 1e-9), 3.0);
                    float s[4];
                    imageops::sampleBilinear(*imgs[i], float(sx), float(sy), s);
                    for (int c = 0; c < 3; ++c) acc[c] += wgt * gain[i] * s[c];
                    acc[3] += wgt * s[3];
                    total += wgt;
                }
                if (total <= 0) continue;
                for (int c = 0; c < 4; ++c) d[c] = float(acc[c] / total);
            }
        });

        if (paramB(AutoCrop)) {
            // The largest rectangle with no empty pixels, found on a coarse grid of the coverage.
            const int cell = std::max(1, std::max(ow, oh) / 512);
            const int gw = std::max(1, ow / cell), gh = std::max(1, oh / cell);
            std::vector<uint8_t> full(size_t(gw) * size_t(gh), 0);
            for (int gy = 0; gy < gh; ++gy)
                for (int gx = 0; gx < gw; ++gx) {
                    bool all = true;
                    for (int y = gy * cell; y < std::min(oh, (gy + 1) * cell) && all; ++y)
                        for (int x = gx * cell; x < std::min(ow, (gx + 1) * cell); ++x)
                            if (res->pixel(size_t(y) * size_t(ow) + size_t(x))[3] < 0.999f) {
                                all = false;
                                break;
                            }
                    full[size_t(gy) * size_t(gw) + size_t(gx)] = all;
                }
            const auto r = largestRectangle(full, gw, gh);
            if (r[2] > 0 && r[3] > 0) {
                const int x0 = r[0] * cell, y0 = r[1] * cell;
                const int cw = std::min(ow - x0, r[2] * cell), ch = std::min(oh - y0, r[3] * cell);
                auto crop = std::make_shared<Image>(cw, ch);
                parallelFor(ch, [&](int y) {
                    std::copy_n(res->pixel(size_t(y + y0) * size_t(ow) + size_t(x0)), size_t(cw) * 4,
                                crop->pixel(size_t(y) * size_t(cw)));
                });
                res = crop;
            }
        }
        finish(ctx, *res);
        out[0] = Value(ImagePtr(res));
    }
};

}  // namespace

void registerPhotoMergeNodes(NodeRegistry& r) {
    r.add<HdrMergeNode>();
    r.add<PanoramaMergeNode>();
}
