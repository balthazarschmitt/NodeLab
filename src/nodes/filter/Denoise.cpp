// Denoise: wavelet noise reduction with Lightroom's Detail-panel controls. Blender's Denoise node
// is Intel's learned OIDN; this is the classic photographic approach (darktable's wavelet
// denoise): no model, no camera profiles, and the same maths on the CPU and the GPU.
//
//  1. Stabilise the noise: scene-linear values go through a signed square root, so photon noise
//     (whose variance grows with the signal) becomes about the same in shadows and highlights.
//     Legacy projects hold gamma-encoded values, which are close to that already.
//  2. Split into luma and chroma (Rec.709 Y, Cb, Cr without offsets): luma noise looks like grain,
//     chroma noise like coloured blotches, and they want different strengths.
//  3. An a-trous wavelet transform (B3 spline, dilated by 2^level) splits each plane into detail
//     bands plus a smooth residual. Each band is shrunk towards zero with a non-negative garrote,
//     at a threshold proportional to the noise expected in that band; the residual is kept, so
//     the mean (exposure) never changes.
//  4. Back to RGB, then undo the stabilisation.
#include <algorithm>
#include <cmath>

#include "core/Parallel.h"
#include "gpu/PointOp.h"
#include "graph/NodeRegistry.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

namespace dn {
enum { Luminance, Detail, Color, ColorDetail };
constexpr int kLumaLevels = 5, kChromaLevels = 6, kMaxLevels = 8;
// Noise (in stabilised units, per full-resolution pixel) at a slider's 100.
constexpr float kSigmaMax = 0.02f;
// The garrote's threshold in noise standard deviations.
constexpr float kK = 3.0f;
// Standard deviation of unit white noise in each B3-spline a-trous band (Starck & Murtagh).
constexpr float kBandNoise[kMaxLevels] = {0.889f, 0.200f, 0.086f, 0.041f, 0.020f, 0.010f, 0.005f, 0.0025f};
constexpr float kTaps[5] = {1.0f / 16, 4.0f / 16, 6.0f / 16, 4.0f / 16, 1.0f / 16};
}  // namespace dn

// Thresholds per level for (Y, Cb, Cr), in the working buffer's pixels.
struct DenoisePlan {
    int levels = 0;
    float t[dn::kMaxLevels][3] = {};
    bool identity() const { return levels == 0; }
};

// Shrinks a band coefficient: d - t^2/d beyond the threshold, 0 within it. Less biased on strong
// edges than soft thresholding (which takes t off everything), so detail keeps its contrast.
inline float garrote(float d, float t) {
    const float shrunk = d - t * t / d;  // computed either way, so loops of it vectorise
    return std::fabs(d) > t ? shrunk : 0.0f;
}

inline float stabilise(float v, bool lin) { return lin ? std::copysign(std::sqrt(std::fabs(v)), v) : v; }
inline float unstabilise(float v, bool lin) { return lin ? std::copysign(v * v, v) : v; }

class DenoiseNode : public Node {
public:
    REFRACTORY_NODE({"filter.denoise", "Denoise", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Luminance", 0.0f, 0.0f, 100.0f), ParamDesc::Float("Detail", 50.0f, 0.0f, 100.0f),
                   ParamDesc::Float("Color", 0.0f, 0.0f, 100.0f), ParamDesc::Float("Color Detail", 50.0f, 0.0f, 100.0f)}})

    // The sliders describe noise in full-resolution pixels. A proxy at `scale` has averaged
    // 1/scale^2 pixels into each of its own, so its noise is `scale` times weaker, and its band j
    // holds what is band j + log2(1/scale) at full resolution: the finest full-resolution bands
    // are below one proxy pixel and simply don't exist there. So the proxy runs fewer levels, with
    // thresholds for the bands they stand for, and approximates the full-resolution result.
    DenoisePlan plan(const EvalContext& ctx) const {
        DenoisePlan p;
        const float lum = paramF(dn::Luminance) * 0.01f, col = paramF(dn::Color) * 0.01f;
        if (lum <= 0.0f && col <= 0.0f) return p;
        const float scale = std::clamp(ctx.scale, 1e-3f, 1.0f);
        const float shift = std::log2(1.0f / scale);
        const int lumaLevels = lum > 0 ? std::max(1, int(std::ceil(dn::kLumaLevels - shift - 1e-4f))) : 0;
        const int chromaLevels = col > 0 ? std::max(1, int(std::ceil(dn::kChromaLevels - shift - 1e-4f))) : 0;
        p.levels = std::max(lumaLevels, chromaLevels);
        const float detail = (paramF(dn::Detail) - 50.0f) / 50.0f, colorDetail = (paramF(dn::ColorDetail) - 50.0f) / 50.0f;
        for (int j = 0; j < p.levels; ++j) {
            const float full = float(j) + shift;  // the full-resolution band this level stands for
            const float noise = dn::kK * dn::kSigmaMax * scale * dn::kBandNoise[j];
            // Detail keeps the finest bands (grain-sized texture); Color Detail keeps the coarse
            // chroma bands (colour along edges) instead of smoothing blotches away.
            const float lumaK = std::exp2(-detail * std::max(0.0f, 1.0f - full / 3.0f));
            const float chromaK = std::exp2(-colorDetail * std::min(1.0f, full / 3.0f));
            p.t[j][0] = j < lumaLevels ? lum * noise * lumaK : 0.0f;
            p.t[j][1] = p.t[j][2] = j < chromaLevels ? col * noise * chromaK : 0.0f;
        }
        return p;
    }

    // Each level blurs with taps 2^j apart, two on each side.
    int roiPadding(const EvalContext& ctx) const override {
        const DenoisePlan p = plan(ctx);
        return p.identity() ? 0 : 2 * ((1 << p.levels) - 1) + 1;
    }

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const DenoisePlan p = plan(ctx);
        if (p.identity()) {
            out[0] = Value(src);
            return;
        }
        const bool lin = ctx.linear();
        const int w = src->w, h = src->h;
        const size_t n = size_t(w) * h;
        // Y, Cb, Cr interleaved: the passes read and write whole rows. Every value is written
        // before it's read, so the buffers (288 MB each at 24 MP) skip zeroing.
        using Buffer = std::vector<float, UninitAllocator<float>>;
        Buffer c(n * 3), tmp(n * 3), acc(n * 3);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                const float* s = src->pixel(i);
                const float r = stabilise(s[0], lin), g = stabilise(s[1], lin), b = stabilise(s[2], lin);
                const float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                c[i * 3] = Y, c[i * 3 + 1] = (b - Y) / 1.8556f, c[i * 3 + 2] = (r - Y) / 1.5748f;
            }
        });
        constexpr float k0 = dn::kTaps[0], k1 = dn::kTaps[1], k2 = dn::kTaps[2], k3 = dn::kTaps[3], k4 = dn::kTaps[4];
        std::vector<float> thr(size_t(w) * 3);  // this level's threshold for every value of a row
        for (int j = 0; j < p.levels; ++j) {
            const int d = 1 << j;
            for (size_t i = 0; i < thr.size(); ++i) thr[i] = p.t[j][i % 3];
            parallelFor(h, [&](int y) {
                const float* row = &c[size_t(y) * w * 3];
                float* o = &tmp[size_t(y) * w * 3];
                // Taps clamp to the edge near the ends of the row; the middle runs as one flat
                // loop over the interleaved values, which vectorises.
                auto clamped = [&](int x) {
                    float s[3] = {0, 0, 0};
                    for (int k = 0; k < 5; ++k) {
                        const float* q = row + size_t(std::clamp(x + (k - 2) * d, 0, w - 1)) * 3;
                        for (int ch = 0; ch < 3; ++ch) s[ch] += dn::kTaps[k] * q[ch];
                    }
                    for (int ch = 0; ch < 3; ++ch) o[x * 3 + ch] = s[ch];
                };
                const int x0 = std::min(w, 2 * d), x1 = std::max(x0, w - 2 * d);
                for (int x = 0; x < x0; ++x) clamped(x);
                for (int x = x1; x < w; ++x) clamped(x);
                const int s1 = 3 * d, s2 = 6 * d;
                for (int i = x0 * 3; i < x1 * 3; ++i)
                    o[i] = k0 * row[i - s2] + k1 * row[i - s1] + k2 * row[i] + k3 * row[i + s1] + k4 * row[i + s2];
            });
            // The vertical half replaces this level with the next in place (it reads `c` only at
            // the value it writes) and adds the band between them, shrunk, into the sum.
            parallelFor(h, [&](int y) {
                const float* r[5];
                for (int k = 0; k < 5; ++k) r[k] = &tmp[size_t(std::clamp(y + (k - 2) * d, 0, h - 1)) * w * 3];
                const size_t base = size_t(y) * w * 3;
                float* cr = &c[base];
                float* ac = &acc[base];
                if (j == 0) {
                    for (int x = 0; x < w * 3; ++x) {
                        const float s = k0 * r[0][x] + k1 * r[1][x] + k2 * r[2][x] + k3 * r[3][x] + k4 * r[4][x];
                        ac[x] = garrote(cr[x] - s, thr[x]);
                        cr[x] = s;
                    }
                } else {
                    for (int x = 0; x < w * 3; ++x) {
                        const float s = k0 * r[0][x] + k1 * r[1][x] + k2 * r[2][x] + k3 * r[3][x] + k4 * r[4][x];
                        ac[x] += garrote(cr[x] - s, thr[x]);
                        cr[x] = s;
                    }
                }
            });
        }
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                const float Y = c[i * 3] + acc[i * 3], cb = c[i * 3 + 1] + acc[i * 3 + 1], cr = c[i * 3 + 2] + acc[i * 3 + 2];
                const float r = Y + 1.5748f * cr, b = Y + 1.8556f * cb;
                const float g = (Y - 0.2126f * r - 0.0722f * b) / 0.7152f;
                float* o = img->pixel(i);
                o[0] = unstabilise(r, lin), o[1] = unstabilise(g, lin), o[2] = unstabilise(b, lin);
                o[3] = src->pixel(i)[3];
            }
        });
        out[0] = Value(ImagePtr(img));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const DenoisePlan p = plan(ctx);
        if (p.identity()) {
            gpu::PointOp op;  // as the CPU's toImage: a channel becomes a grey image
            op.body = "    out0 = img0(p);\n";
            gpu::runOver(ctx, *this, op, in, out);
            return;
        }
        int w, h;
        in[0].size(w, h);
        const float lin = ctx.linear() ? 1.0f : 0.0f;
        const std::string functions = R"(
float stab(float v) { return P[0] != 0.0 ? sign(v) * sqrt(abs(v)) : v; }
float unstab(float v) { return P[0] != 0.0 ? sign(v) * v * v : v; }
float garrote(float d, float t) { return abs(d) > t ? d - t * t / d : 0.0; }
)";
        // Y, Cb, Cr and alpha.
        gpu::PointOp enc;
        enc.w = w, enc.h = h, enc.full = true;
        enc.functions = functions;
        enc.params = {lin};
        enc.body = R"(
    vec4 s = img0(p);
    float r = stab(s.r), g = stab(s.g), b = stab(s.b);
    float Y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    out0 = vec4(Y, (b - Y) / 1.8556, (r - Y) / 1.5748, s.a);
)";
        Value c = gpu::runPass(ctx, enc, {in[0]}, {true})[0], acc;
        for (int j = 0; j < p.levels; ++j) {
            const float d = float(1 << j);
            gpu::PointOp hp;
            hp.w = w, hp.h = h, hp.full = true;
            hp.gather = {0};
            hp.params = {d};
            hp.body = R"(
    int d = int(P[0]);
    out0 = (fetch0(p + ivec2(-2 * d, 0)) + fetch0(p + ivec2(2 * d, 0))) * (1.0 / 16.0)
         + (fetch0(p + ivec2(-d, 0)) + fetch0(p + ivec2(d, 0))) * (4.0 / 16.0) + fetch0(p) * (6.0 / 16.0);
)";
            const Value hv = gpu::runPass(ctx, hp, {c}, {true})[0];
            // The vertical half, then the band (this level minus the next) shrunk into the sum.
            gpu::PointOp vp;
            vp.w = w, vp.h = h, vp.full = true;
            vp.gather = {0};
            vp.functions = functions;
            vp.params = {lin, d, p.t[j][0], p.t[j][1], p.t[j][2]};
            vp.body = R"(
    int d = int(P[1]);
    vec4 s = (fetch0(p + ivec2(0, -2 * d)) + fetch0(p + ivec2(0, 2 * d))) * (1.0 / 16.0)
           + (fetch0(p + ivec2(0, -d)) + fetch0(p + ivec2(0, d))) * (4.0 / 16.0) + fetch0(p) * (6.0 / 16.0);
    vec4 band = img1(p) - s;
    vec4 a = has2 ? img2(p) : vec4(0.0);
    out0 = s;
    out1 = a + vec4(garrote(band.x, P[2]), garrote(band.y, P[3]), garrote(band.z, P[4]), 0.0);
)";
            std::vector<Value> r = gpu::runPass(ctx, vp, {hv, c, acc}, {true, true});
            c = r[0], acc = r[1];
        }
        gpu::PointOp fin;
        fin.functions = functions;
        fin.params = {lin};
        fin.body = R"(
    vec4 v = img0(p) + img1(p);
    float r = v.x + 1.5748 * v.z, b = v.x + 1.8556 * v.y;
    float g = (v.x - 0.2126 * r - 0.0722 * b) / 0.7152;
    out0 = vec4(unstab(r), unstab(g), unstab(b), img2(p).a);
)";
        fin.w = w, fin.h = h;
        gpu::runPoint(ctx, *this, fin, {c, acc, in[0]}, out);
    }
};

}  // namespace

void registerDenoiseNode(NodeRegistry& r) { r.add<DenoiseNode>(); }
