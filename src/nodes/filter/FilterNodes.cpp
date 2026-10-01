// Filter nodes: blurs, convolution filters, morphology, stylization, glare.
// Sizes are in full-resolution pixels; ctx.scale converts them for the preview proxy.
#include <cmath>

#include "gpu/Blur.h"
#include "gpu/PointOp.h"
#include "nodes/ImageOps.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;
using namespace imageops;

namespace {

constexpr float kPi = 3.14159265f;

std::shared_ptr<Image> copyOf(const Image& src) { return std::make_shared<Image>(src); }

// ---- GPU helpers

// A single-output op over `src`'s pixels that reads the given pins anywhere.
gpu::PointOp gatherOp(const Value& src, std::string body, std::vector<float> params, std::vector<int> gather = {0}) {
    gpu::PointOp op;
    src.size(op.w, op.h);
    op.body = std::move(body);
    op.params = std::move(params);
    op.gather = std::move(gather);
    return op;
}

// A GPU value's texture (running it if pending), and a texture as an image value.
gpu::TexturePtr textureOf(const Value& v) {
    if (auto i = std::get_if<GpuImagePtr>(&v.v); i && *i) return (*i)->texture();
    if (auto c = std::get_if<GpuChannelPtr>(&v.v); c && *c) return (*c)->texture();
    throw gpu::Error("GPU: expected pixels on the device");
}
Value imageValue(gpu::TexturePtr t) {
    auto r = std::make_shared<GpuImage>();
    r->w = t->w(), r->h = t->h(), r->tex = std::move(t);
    return Value(GpuImagePtr(r));
}

// ---------------------------------------------------------------- blurs

class BlurNode : public Node {
public:
    NODELAB_NODE({"filter.blur", "Blur", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Size X", 10.0f, 0.0f, 200.0f).when(2, 0), ParamDesc::Float("Size Y", 10.0f, 0.0f, 200.0f).when(2, 0),
                   ParamDesc::Bool("Relative", false), ParamDesc::Enum("Aspect Correction", 0, {"None", "Y", "X"}).when(2),
                   ParamDesc::Float("Factor X", 1.0f, 0.0f, 100.0f).when(2), ParamDesc::Float("Factor Y", 1.0f, 0.0f, 100.0f).when(2)}})
    // Blur sizes (pixels) for an image of w x h.
    void sizes(const EvalContext& ctx, int w, int h, float& sx, float& sy) const {
        sx = paramF(0) * ctx.scale;
        sy = paramF(1) * ctx.scale;
        if (paramB(2)) {
            // Blender's Relative: percent of the image size, so one setting fits any resolution
            // (the working image is already proxy-sized, so no ctx.scale). Aspect Correction Y
            // measures Y against the width too (round blur on non-square images), X the reverse.
            const int ac = paramI(3);
            sx = paramF(4) * 0.01f * float(ac == 2 ? h : w);
            sy = paramF(5) * 0.01f * float(ac == 1 ? w : h);
        }
    }
    int roiPadding(const EvalContext& ctx) const override {
        const PixelFrame fr = frameOf(ctx, ctx.defaultW, ctx.defaultH);
        float sx, sy;
        sizes(ctx, fr.fullW, fr.fullH, sx, sy);
        return std::max(blurReach(sx * 0.5f), blurReach(sy * 0.5f)) + 1;
    }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        // A channel (e.g. a mask) is blurred as one plane and passed on as a channel: converting
        // it to a grey RGBA image first would blur four identical planes. Consumers convert it
        // back to an image exactly as they would have (grey, alpha 1), so results don't change.
        const auto* chp = std::get_if<ChannelPtr>(&in[0].v);
        const Channel* srcCh = chp && *chp && !(*chp)->constant ? chp->get() : nullptr;
        if (chp && *chp && (*chp)->constant) {
            out[0] = in[0];  // flat: nothing to blur
            return;
        }
        ImagePtr src = srcCh ? nullptr : toImage(in[0], 0, 0);
        if (!src && !srcCh) return;
        const int w = srcCh ? srcCh->w : src->w, h = srcCh ? srcCh->h : src->h;
        const PixelFrame fr = frameOf(ctx, w, h);
        float sx, sy;
        sizes(ctx, fr.fullW, fr.fullH, sx, sy);
        if (srcCh) {
            auto c = std::make_shared<Channel>(*srcCh);
            blurChannel(c->data, w, h, sx * 0.5f, sy * 0.5f);
            out[0] = Value(ChannelPtr(c));
            return;
        }
        auto img = copyOf(*src);
        blurImage(*img, sx * 0.5f, sy * 0.5f);
        out[0] = Value(ImagePtr(img));
    }

    // Images and sized channels (numbers and flat channels have nothing to blur).
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const Value v = in[0].onGpu() ? in[0] : gpu::toGpu(in[0], ctx.gpuHalf);
        int w, h;
        v.size(w, h);
        const PixelFrame fr = frameOf(ctx, w, h);
        float sx, sy;
        sizes(ctx, fr.fullW, fr.fullH, sx, sy);
        if (auto c = std::get_if<GpuChannelPtr>(&v.v)) {
            auto r = std::make_shared<GpuChannel>();
            r->tex = gpu::boxBlur((*c)->texture(), sx * 0.5f, sy * 0.5f), r->w = w, r->h = h;
            out[0] = Value(GpuChannelPtr(r));
        } else if (auto i = std::get_if<GpuImagePtr>(&v.v)) {
            auto r = std::make_shared<GpuImage>();
            r->tex = gpu::boxBlur((*i)->texture(), sx * 0.5f, sy * 0.5f), r->w = w, r->h = h;
            out[0] = Value(GpuImagePtr(r));
        }
    }
};

class DirectionalBlurNode : public Node {
public:
    NODELAB_NODE({"filter.directional_blur", "Directional Blur", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Distance", 30.0f, 0.0f, 400.0f), ParamDesc::Float("Angle", 0.0f, -180.0f, 180.0f),
                   ParamDesc::Float("Spin", 0.0f, -90.0f, 90.0f), ParamDesc::Float("Zoom", 0.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Center X", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Center Y", 0.5f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float dist = paramF(0) * ctx.scale, ang = paramF(1) * kPi / 180.0f;
        const float spin = paramF(2) * kPi / 180.0f, zoom = paramF(3);
        const float cx = paramF(4) * src->w, cy = paramF(5) * src->h;
        const int steps = std::clamp(int(std::max({dist, std::fabs(spin) * src->w * 0.5f, zoom * src->w * 0.5f})), 1, 96);
        const float dx = std::cos(ang) * dist, dy = std::sin(ang) * dist;
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float*, float* d) {
            float acc[4] = {0, 0, 0, 0};
            const float px = x + 0.5f - cx, py = y + 0.5f - cy;
            for (int s = 0; s < steps; ++s) {
                float t = steps > 1 ? float(s) / (steps - 1) : 0.0f;
                float a = spin * t, sc = 1.0f - zoom * t;
                float rx = (px * std::cos(a) - py * std::sin(a)) * sc + cx + dx * t;
                float ry = (px * std::sin(a) + py * std::cos(a)) * sc + cy + dy * t;
                float c[4];
                sampleBilinear(*src, rx, ry, c);
                for (int k = 0; k < 4; ++k) acc[k] += c[k];
            }
            for (int k = 0; k < 4; ++k) d[k] = acc[k] / steps;
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        in[0].size(w, h);
        const float dist = paramF(0) * ctx.scale, ang = paramF(1) * kPi / 180.0f;
        const float spin = paramF(2) * kPi / 180.0f, zoom = paramF(3);
        const int steps = std::clamp(int(std::max({dist, std::fabs(spin) * w * 0.5f, zoom * w * 0.5f})), 1, 96);
        gpu::PointOp op = gatherOp(in[0], R"(
    int steps = int(P[0]);
    vec2 c = vec2(P[4], P[5]) * vec2(size0);
    vec2 q = vec2(p) + 0.5 - c;
    vec4 acc = vec4(0.0);
    for (int s = 0; s < steps; ++s) {
        float t = steps > 1 ? float(s) / float(steps - 1) : 0.0;
        float a = P[1] * t, sc = 1.0 - P[2] * t;
        float ca = cos(a), sa = sin(a);
        acc += bilinear0(vec2(q.x * ca - q.y * sa, q.x * sa + q.y * ca) * sc + c + vec2(P[6], P[7]) * t, false);
    }
    out0 = acc / float(steps);
)",
                                   {float(steps), spin, zoom, 0.0f, paramF(4), paramF(5), std::cos(ang) * dist, std::sin(ang) * dist});
        op.inlinable = false;
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

class BilateralBlurNode : public Node {
public:
    NODELAB_NODE({"filter.bilateral_blur", "Bilateral Blur", "Filter",
                  {{"Image", PinType::Image}, {"Determinator", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Radius", 8.0f, 0.0f, 60.0f), ParamDesc::Float("Color Sigma", 0.1f, 0.001f, 1.0f)}})
    int roiPadding(const EvalContext& ctx) const override { return int(std::ceil(paramF(0) * ctx.scale)) + 1; }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ImagePtr det = toImage(in[1], src->w, src->h);
        if (!det || det->w != src->w || det->h != src->h) det = src;  // edges come from the image itself
        const float r = paramF(0) * ctx.scale;
        const float cs = paramF(1);
        const int ri = int(std::ceil(r));
        const int step = std::max(1, ri / 7);  // cap the kernel at ~15x15 taps
        const float inv2s2 = 1.0f / (2.0f * std::max(r * 0.5f, 0.5f) * std::max(r * 0.5f, 0.5f));
        const float inv2c2 = 1.0f / (2.0f * cs * cs);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            const float* c0 = det->pixel(size_t(y) * src->w + x);
            float acc[4] = {0, 0, 0, 0}, wsum = 0;
            for (int j = -ri; j <= ri; j += step)
                for (int i = -ri; i <= ri; i += step) {
                    int xx = std::clamp(x + i, 0, src->w - 1), yy = std::clamp(y + j, 0, src->h - 1);
                    size_t idx = size_t(yy) * src->w + xx;
                    const float* c = det->pixel(idx);
                    float dc = (c[0] - c0[0]) * (c[0] - c0[0]) + (c[1] - c0[1]) * (c[1] - c0[1]) + (c[2] - c0[2]) * (c[2] - c0[2]);
                    float w = std::exp(-float(i * i + j * j) * inv2s2 - dc * inv2c2);
                    const float* p = src->pixel(idx);
                    for (int k = 0; k < 4; ++k) acc[k] += p[k] * w;
                    wsum += w;
                }
            for (int k = 0; k < 4; ++k) d[k] = wsum > 0 ? acc[k] / wsum : s[k];
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h, dw = 0, dh = 0;
        in[0].size(w, h);
        // The edges come from the image itself unless a determinator of its size is connected.
        const bool det = in[1].size(dw, dh) && dw == w && dh == h;
        const float r = paramF(0) * ctx.scale, cs = paramF(1);
        const int ri = int(std::ceil(r));
        const float sr = std::max(r * 0.5f, 0.5f);
        gpu::PointOp op = gatherOp(in[0], R"(
    int ri = int(P[0]), step = int(P[1]);
    bool det = P[4] != 0.0;
    vec3 c0 = det ? fetch1(p).rgb : fetch0(p).rgb;
    vec4 acc = vec4(0.0);
    float wsum = 0.0;
    for (int j = -ri; j <= ri; j += step)
        for (int i = -ri; i <= ri; i += step) {
            ivec2 q = p + ivec2(i, j);
            vec3 c = (det ? fetch1(q).rgb : fetch0(q).rgb) - c0;
            float wt = exp(-float(i * i + j * j) * P[2] - dot(c, c) * P[3]);
            acc += fetch0(q) * wt;
            wsum += wt;
        }
    out0 = wsum > 0.0 ? acc / wsum : fetch0(p);
)",
                                   {float(ri), float(std::max(1, ri / 7)), 1.0f / (2.0f * sr * sr), 1.0f / (2.0f * cs * cs), det ? 1.0f : 0.0f},
                                   {0, 1});
        op.inlinable = false;
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

// ---------------------------------------------------------------- 3x3 filters

class FilterNode : public Node {
public:
    enum { Soften, BoxSharpen, DiamondSharpen, Laplace, Sobel, Prewitt, Kirsch, Shadow };
    NODELAB_NODE({"filter.filter", "Filter", "Filter",
                  {{"Image", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Enum("Type", Sobel, {"Soften", "Box Sharpen", "Diamond Sharpen", "Laplace", "Sobel", "Prewitt",
                                                   "Kirsch", "Shadow"})}})
    int roiPadding(const EvalContext&) const override { return 1; }  // 3x3 kernels
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int type = paramI(1);
        ChannelPtr fac = channelOr(in[1], 1.0f);
        ChannelSampler sf = paramSampler(*this, 1, fac, src->w, src->h);
        auto at = [&](int x, int y, int k) {
            return src->pixel(size_t(std::clamp(y, 0, src->h - 1)) * src->w + std::clamp(x, 0, src->w - 1))[k];
        };
        // Kernels as 3x3 rows; edge detectors use the magnitude of a kernel and its transpose.
        static const float soften[9] = {1, 2, 1, 2, 4, 2, 1, 2, 1};
        static const float box[9] = {-1, -1, -1, -1, 9, -1, -1, -1, -1};
        static const float diamond[9] = {0, -1, 0, -1, 5, -1, 0, -1, 0};
        static const float laplace[9] = {-1, -1, -1, -1, 8, -1, -1, -1, -1};
        static const float sobel[9] = {-1, 0, 1, -2, 0, 2, -1, 0, 1};
        static const float prewitt[9] = {-1, 0, 1, -1, 0, 1, -1, 0, 1};
        static const float kirsch[9] = {-3, -3, 5, -3, 0, 5, -3, -3, 5};
        static const float shadow[9] = {1, 2, 1, 0, 1, 0, -1, -2, -1};
        auto conv = [&](const float* kr, int x, int y, int k, bool transpose) {
            float s = 0;
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i) s += kr[transpose ? i * 3 + j : j * 3 + i] * at(x + i - 1, y + j - 1, k);
            return s;
        };
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            for (int k = 0; k < 3; ++k) {
                float v;
                switch (type) {
                    case Soften: v = conv(soften, x, y, k, false) / 16.0f; break;
                    case BoxSharpen: v = conv(box, x, y, k, false); break;
                    case DiamondSharpen: v = conv(diamond, x, y, k, false); break;
                    case Laplace: v = conv(laplace, x, y, k, false) / 8.0f + 0.0f; break;
                    case Sobel: v = std::hypot(conv(sobel, x, y, k, false), conv(sobel, x, y, k, true)) * 0.25f; break;
                    case Prewitt: v = std::hypot(conv(prewitt, x, y, k, false), conv(prewitt, x, y, k, true)) / 3.0f; break;
                    case Kirsch: v = std::hypot(conv(kirsch, x, y, k, false), conv(kirsch, x, y, k, true)) / 15.0f; break;
                    default: v = conv(shadow, x, y, k, false) + 0.0f; break;
                }
                float f = sf(x, y);
                d[k] = clampColor(ctx.linear(), s[k] + (v - s[k]) * f);
            }
            d[3] = s[3];
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op = gatherOp(in[0], R"(
    const float K[72] = float[72](1, 2, 1, 2, 4, 2, 1, 2, 1,  -1, -1, -1, -1, 9, -1, -1, -1, -1,
                                  0, -1, 0, -1, 5, -1, 0, -1, 0,  -1, -1, -1, -1, 8, -1, -1, -1, -1,
                                  -1, 0, 1, -2, 0, 2, -1, 0, 1,  -1, 0, 1, -1, 0, 1, -1, 0, 1,
                                  -3, -3, 5, -3, 0, 5, -3, -3, 5,  1, 2, 1, 0, 1, 0, -1, -2, -1);
    int type = int(P[0]), base = type * 9;
    vec3 a = vec3(0.0), b = vec3(0.0);  // the kernel and its transpose
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            vec3 v = fetch0(p + ivec2(i - 1, j - 1)).rgb;
            a += K[base + j * 3 + i] * v;
            b += K[base + i * 3 + j] * v;
        }
    vec3 v;
    if (type == 0) v = a / 16.0;
    else if (type == 3) v = a / 8.0;
    else if (type == 4) v = sqrt(a * a + b * b) * 0.25;
    else if (type == 5) v = sqrt(a * a + b * b) / 3.0;
    else if (type == 6) v = sqrt(a * a + b * b) / 15.0;
    else v = a;
    vec4 s = fetch0(p);
    out0 = vec4(clampColor(uLinear, s.rgb + (v - s.rgb) * par1(p)), s.a);
)", {float(paramI(1))});
        op.defaults = {NAN, 1.0f};
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

// ---------------------------------------------------------------- morphology

class DilateErodeNode : public Node {
public:
    NODELAB_NODE({"filter.dilate_erode", "Dilate / Erode", "Filter",
                  {{"Mask", PinType::Channel}},
                  {{"Mask", PinType::Channel}},
                  {ParamDesc::Enum("Mode", 0, {"Distance", "Feather"}), ParamDesc::Float("Distance", 5.0f, -100.0f, 100.0f)}})
    // Only distances up to Distance matter: anything farther gives the same result.
    int roiPadding(const EvalContext& ctx) const override { return int(std::ceil(std::fabs(paramF(1) * ctx.scale))) + 2; }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ChannelPtr m = toChannel(in[0]);
        if (!m || m->constant) {
            if (m) out[0] = Value(m);
            return;
        }
        const int w = m->w, h = m->h;
        const float dist = paramF(1) * ctx.scale;
        const bool feather = paramI(0) == 1;
        std::vector<uint8_t> inside(size_t(w) * h), outside(size_t(w) * h);
        for (size_t i = 0; i < inside.size(); ++i) {
            inside[i] = m->data[i] >= 0.5f;
            outside[i] = !inside[i];
        }
        // Dilate: grow by distance to the mask. Erode: shrink by distance to the background.
        std::vector<float> dt = distanceTransform(dist >= 0 ? inside : outside, w, h);
        const float ad = std::max(std::fabs(dist), 1e-3f);
        auto res = std::make_shared<Channel>(Channel::makeSized(w, h));
        for (size_t i = 0; i < dt.size(); ++i) {
            float v;
            if (dist >= 0) v = feather ? std::max(m->data[i], clamp01(1.0f - dt[i] / ad)) : (dt[i] <= ad ? 1.0f : 0.0f);
            else v = feather ? std::min(m->data[i], clamp01(dt[i] / ad)) : (dt[i] > ad ? 1.0f : 0.0f);
            res->data[i] = v;
        }
        out[0] = Value(ChannelPtr(res));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const float dist = paramF(1) * ctx.scale, ad = std::max(std::fabs(dist), 1e-3f);
        // The exact Euclidean distance transform, but only out to Distance (anything farther gives
        // the same result): per column the nearest seed within reach, then per row the nearest of
        // those. Seeds are mask pixels when dilating, background ones when eroding.
        const float reach = std::ceil(ad) + 1.0f;
        gpu::PointOp col = gatherOp(in[0], R"(
    int r = int(P[0]);
    float best = 1e4;
    for (int dy = -r; dy <= r; ++dy) {
        int y = p.y + dy;
        if (y < 0 || y >= size0.y) continue;
        bool inside = fetchCh0(ivec2(p.x, y)) >= 0.5;
        if (inside == (P[1] != 0.0)) best = min(best, abs(float(dy)));
    }
    out0 = best;
)", {reach, dist >= 0 ? 1.0f : 0.0f});
        std::vector<Value> g = gpu::runPass(ctx, col, {in[0]}, {false});
        gpu::PointOp row = gatherOp(in[0], R"(
    int r = int(P[0]);
    float best = 1e8;
    for (int dx = -r; dx <= r; ++dx) {
        int x = p.x + dx;
        if (x < 0 || x >= size1.x) continue;
        float gy = fetchCh1(ivec2(x, p.y));
        best = min(best, float(dx * dx) + gy * gy);
    }
    float dt = sqrt(best), m = ch0(p), ad = P[2];
    bool feather = P[3] != 0.0;
    if (P[1] != 0.0) out0 = feather ? max(m, clamp01(1.0 - dt / ad)) : (dt <= ad ? 1.0 : 0.0);
    else out0 = feather ? min(m, clamp01(dt / ad)) : (dt > ad ? 1.0 : 0.0);
)", {reach, dist >= 0 ? 1.0f : 0.0f, ad, paramI(0) == 1 ? 1.0f : 0.0f}, {1});
        gpu::runPoint(ctx, *this, row, {in[0], g[0]}, out);
    }
};

// ---------------------------------------------------------------- stylize

class KuwaharaNode : public Node {
public:
    NODELAB_NODE({"filter.kuwahara", "Kuwahara", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Size", 6.0f, 1.0f, 50.0f)}})
    int roiPadding(const EvalContext& ctx) const override {
        return std::max(1, int(std::round(paramF(0) * ctx.scale))) + 1;
    }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int w = src->w, h = src->h;
        const int r = std::max(1, int(std::round(paramF(0) * ctx.scale)));
        // Summed-area tables of r, g, b, luminance and luminance^2 for O(1) region stats.
        const size_t W = size_t(w) + 1;
        std::vector<double> sat[5];
        for (auto& t : sat) t.assign(W * (h + 1), 0.0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float* p = src->pixel(size_t(y) * w + x);
                double l = luminance(p[0], p[1], p[2]);
                double v[5] = {p[0], p[1], p[2], l, l * l};
                for (int k = 0; k < 5; ++k)
                    sat[k][(y + 1) * W + x + 1] = v[k] + sat[k][y * W + x + 1] + sat[k][(y + 1) * W + x] - sat[k][y * W + x];
            }
        auto region = [&](int k, int x0, int y0, int x1, int y1) {  // inclusive-exclusive, clamped
            x0 = std::clamp(x0, 0, w), x1 = std::clamp(x1, 0, w), y0 = std::clamp(y0, 0, h), y1 = std::clamp(y1, 0, h);
            return sat[k][y1 * W + x1] - sat[k][y0 * W + x1] - sat[k][y1 * W + x0] + sat[k][y0 * W + x0];
        };
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            const int qx[4][2] = {{x - r, x + 1}, {x, x + r + 1}, {x - r, x + 1}, {x, x + r + 1}};
            const int qy[4][2] = {{y - r, y + 1}, {y - r, y + 1}, {y, y + r + 1}, {y, y + r + 1}};
            double best = 1e30;
            for (int q = 0; q < 4; ++q) {
                int x0 = std::clamp(qx[q][0], 0, w), x1 = std::clamp(qx[q][1], 0, w);
                int y0 = std::clamp(qy[q][0], 0, h), y1 = std::clamp(qy[q][1], 0, h);
                double n = double(x1 - x0) * (y1 - y0);
                if (n <= 0) continue;
                double mean = region(3, x0, y0, x1, y1) / n;
                double var = region(4, x0, y0, x1, y1) / n - mean * mean;
                if (var < best) {
                    best = var;
                    for (int k = 0; k < 3; ++k) d[k] = float(region(k, x0, y0, x1, y1) / n);
                }
            }
            d[3] = s[3];
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int r = std::max(1, int(std::round(paramF(0) * ctx.scale)));
        // Window sums of r, g, b, luminance and its square along rows, over the r + 1 pixels ending
        // (L) and starting (R) at each pixel, clamped to the image as the CPU's table lookups are.
        gpu::PointOp rows = gatherOp(in[0], R"(
    int r = int(P[0]);
    vec4 lo = vec4(0.0), hi = vec4(0.0);
    vec2 sq = vec2(0.0);
    for (int i = 0; i <= r; ++i) {
        int a = p.x - i, b = p.x + i;
        if (a >= 0) { vec3 c = fetch0(ivec2(a, p.y)).rgb; float l = luminance(c); lo += vec4(c, l); sq.x += l * l; }
        if (b < size0.x) { vec3 c = fetch0(ivec2(b, p.y)).rgb; float l = luminance(c); hi += vec4(c, l); sq.y += l * l; }
    }
    out0 = lo;
    out1 = hi;
    out2 = vec4(sq, 0.0, 0.0);
)", {float(r)});
        rows.full = true;
        std::vector<Value> sums = gpu::runPass(ctx, rows, {in[0]}, {true, true, true});
        // Down the columns, the four quadrants' sums; the one with the least luminance variance
        // gives its mean colour.
        gpu::PointOp pick = gatherOp(in[0], R"(
    int r = int(P[0]);
    vec4 s[4] = vec4[4](vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));  // TL TR BL BR
    vec4 q2 = vec4(0.0);
    for (int i = 0; i <= r; ++i) {
        int a = p.y - i, b = p.y + i;
        if (a >= 0) { ivec2 c = ivec2(p.x, a); s[0] += fetch1(c); s[1] += fetch2(c); q2.xy += fetch3(c).xy; }
        if (b < size0.y) { ivec2 c = ivec2(p.x, b); s[2] += fetch1(c); s[3] += fetch2(c); q2.zw += fetch3(c).xy; }
    }
    float nl = float(min(p.x, r) + 1), nr = float(min(size0.x - 1 - p.x, r) + 1);
    float nt = float(min(p.y, r) + 1), nb = float(min(size0.y - 1 - p.y, r) + 1);
    float n[4] = float[4](nl * nt, nr * nt, nl * nb, nr * nb);
    float best = 1e30;
    vec3 col = vec3(0.0);
    for (int q = 0; q < 4; ++q) {
        float mean = s[q].a / n[q];
        float var = q2[q] / n[q] - mean * mean;
        if (var < best) { best = var; col = s[q].rgb / n[q]; }
    }
    out0 = vec4(col, fetch0(p).a);
)", {float(r)}, {0, 1, 2, 3});
        pick.inlinable = false;
        gpu::runPoint(ctx, *this, pick, {in[0], sums[0], sums[1], sums[2]}, out);
    }
};

class PixelateNode : public Node {
public:
    NODELAB_NODE({"filter.pixelate", "Pixelate", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Size", 16.0f, 1.0f, 200.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int bs = std::max(1, int(std::round(paramF(0) * ctx.scale)));
        const int w = src->w, h = src->h;
        auto img = std::make_shared<Image>(w, h);
        const int bw = (w + bs - 1) / bs, bh = (h + bs - 1) / bs;
        parallelFor(bh, [&](int by) {
            for (int bx = 0; bx < bw; ++bx) {
                int x0 = bx * bs, y0 = by * bs, x1 = std::min(w, x0 + bs), y1 = std::min(h, y0 + bs);
                float acc[4] = {0, 0, 0, 0};
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                        for (int k = 0; k < 4; ++k) acc[k] += src->pixel(size_t(y) * w + x)[k];
                float inv = 1.0f / float((x1 - x0) * (y1 - y0));
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                        for (int k = 0; k < 4; ++k) img->pixel(size_t(y) * w + x)[k] = acc[k] * inv;
            }
        });
        out[0] = Value(ImagePtr(img));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        in[0].size(w, h);
        const int bs = std::max(1, int(std::round(paramF(0) * ctx.scale)));
        const int bw = (w + bs - 1) / bs, bh = (h + bs - 1) / bs;
        // Sums of each block's part of every row, then of those down each block.
        gpu::PointOp rows = gatherOp(in[0], R"(
    int bs = int(P[0]), x0 = p.x * bs, x1 = min(size0.x, x0 + bs);
    vec4 acc = vec4(0.0);
    for (int x = x0; x < x1; ++x) acc += fetch0(ivec2(x, p.y));
    out0 = acc;
)", {float(bs)});
        rows.w = bw, rows.full = true;
        std::vector<Value> r = gpu::runPass(ctx, rows, {in[0]}, {true});
        gpu::PointOp blocks = gatherOp(r[0], R"(
    int bs = int(P[0]), y0 = p.y * bs, y1 = min(int(P[1]), y0 + bs);
    vec4 acc = vec4(0.0);
    for (int y = y0; y < y1; ++y) acc += fetch0(ivec2(p.x, y));
    out0 = acc;
)", {float(bs), float(h)});
        blocks.h = bh, blocks.full = true;
        std::vector<Value> b = gpu::runPass(ctx, blocks, r, {true});
        gpu::PointOp fill = gatherOp(in[0], R"(
    int bs = int(P[0]);
    ivec2 blk = p / bs, o = blk * bs;
    ivec2 n = min(uSize, o + bs) - o;
    out0 = fetch0(blk) * (1.0 / float(n.x * n.y));
)", {float(bs)});
        gpu::runPoint(ctx, *this, fill, b, out);
    }
};

class PosterizeNode : public Node {
public:
    NODELAB_NODE({"filter.posterize", "Posterize", "Filter",
                  {{"Image", PinType::Image}, {"Steps", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Steps", 6.0f, 2.0f, 64.0f)}})
    int roiPadding(const EvalContext&) const override { return 0; }  // per pixel
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        ChannelPtr st = channelOr(in[1], 6.0f);
        ChannelSampler ss = paramSampler(*this, 1, st, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float n = std::max(2.0f, std::round(ss(x, y))) - 1.0f;
            for (int k = 0; k < 3; ++k) d[k] = std::round(clamp01(s[k]) * n) / n;
            d[3] = s[3];
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op;
        // floor(x + 0.5) is std::round for x >= 0 (GLSL's round may go either way at .5).
        op.body = R"(
    vec4 s = img0(p);
    float n = max(2.0, floor(par1(p) + 0.5)) - 1.0;
    out0 = vec4(floor(clamp01(s.rgb) * n + 0.5) / n, s.a);
)";
        op.defaults = {NAN, 6.0f};
        gpu::runOver(ctx, *this, op, in, out);
    }
};

// ---------------------------------------------------------------- light effects

class GlareNode : public Node {
public:
    enum { FogGlow, Streaks, SimpleStar };
    NODELAB_NODE({"filter.glare", "Glare", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}, {"Glare", PinType::Image}},
                  {ParamDesc::Enum("Type", FogGlow, {"Fog Glow", "Streaks", "Simple Star"}),
                   ParamDesc::Float("Threshold", 0.75f, 0.0f, 1.0f), ParamDesc::Float("Size", 60.0f, 1.0f, 500.0f),
                   ParamDesc::Float("Strength", 1.0f, 0.0f, 4.0f), ParamDesc::Float("Streaks", 4.0f, 2.0f, 16.0f),
                   ParamDesc::Float("Angle", 30.0f, 0.0f, 180.0f), ParamDesc::Float("Fade", 0.9f, 0.5f, 0.99f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int type = paramI(0);
        const float thr = paramF(1), size = paramF(2) * ctx.scale, strength = paramF(3);
        const int w = src->w, h = src->h;

        // Highlights above the threshold, rescaled so the brightest parts reach 1.
        auto hi = mapImage(*src, [&](int, int, const float* s, float* d) {
            for (int k = 0; k < 3; ++k) d[k] = std::max(s[k] - thr, 0.0f) / std::max(1.0f - thr, 1e-3f);
            d[3] = 1.0f;
        });

        std::shared_ptr<Image> glare;
        if (type == FogGlow) {
            // Two blur widths summed give a tight core plus a wide halo.
            auto wide = std::make_shared<Image>(*hi);
            blurImage(*wide, size * 0.5f, size * 0.5f);
            blurImage(*hi, size * 0.12f, size * 0.12f);
            glare = mapImage(*wide, [&](int x, int y, const float* a, float* d) {
                const float* b = hi->pixel(size_t(y) * w + x);
                for (int k = 0; k < 3; ++k) d[k] = a[k] * 0.7f + b[k] * 0.6f;
                d[3] = 1.0f;
            });
        } else {
            // Streaks: for each direction, repeatedly add a shifted, faded copy with doubling offsets
            // (exponential tail in log2(length) passes).
            const int n = type == SimpleStar ? 4 : std::max(2, int(paramF(4)));
            const float angle0 = paramF(5) * kPi / 180.0f, fade = paramF(6);
            glare = std::make_shared<Image>(w, h);
            for (int s = 0; s < n; ++s) {
                float a = angle0 + (type == SimpleStar ? s * kPi / 4.0f : s * 2.0f * kPi / n);
                float dx = std::cos(a), dy = std::sin(a);
                auto cur = std::make_shared<Image>(*hi);
                for (float step = 1.0f; step < size; step *= 2.0f) {
                    float wgt = std::pow(fade, step);
                    auto next = mapImage(*cur, [&](int x, int y, const float* c, float* d) {
                        float o[4], o2[4];
                        sampleBilinear(*cur, x + 0.5f - dx * step, y + 0.5f - dy * step, o, true);
                        sampleBilinear(*cur, x + 0.5f + dx * step, y + 0.5f + dy * step, o2, true);
                        for (int k = 0; k < 3; ++k) d[k] = c[k] + (o[k] + o2[k]) * wgt;
                        d[3] = 1.0f;
                    });
                    cur = next;
                }
                for (size_t i = 0; i < glare->pixelCount(); ++i)
                    for (int k = 0; k < 3; ++k) glare->pixel(i)[k] += cur->pixel(i)[k] / (n * std::max(1.0f, std::log2(size)));
            }
        }
        auto result = mapImage(*src, [&](int x, int y, const float* s, float* d) {
            const float* gl = glare->pixel(size_t(y) * w + x);
            for (int k = 0; k < 3; ++k) d[k] = clampColor(ctx.linear(), s[k] + gl[k] * strength);
            d[3] = s[3];
        });
        for (size_t i = 0; i < glare->pixelCount(); ++i) {
            for (int k = 0; k < 3; ++k) glare->pixel(i)[k] = clampColor(ctx.linear(), glare->pixel(i)[k] * strength);
            glare->pixel(i)[3] = 1.0f;
        }
        out[0] = Value(ImagePtr(result));
        out[1] = Value(ImagePtr(glare));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        in[0].size(w, h);
        const int type = paramI(0);
        const float thr = paramF(1), size = paramF(2) * ctx.scale, strength = paramF(3);
        gpu::PointOp hiOp;
        hiOp.w = w, hiOp.h = h;
        hiOp.body = "    out0 = vec4(max(img0(p).rgb - P[0], 0.0) / max(1.0 - P[0], 1e-3), 1.0);\n";
        hiOp.params = {thr};
        hiOp.full = true;
        const Value hi = gpu::runPass(ctx, hiOp, {in[0]}, {true})[0];
        gpu::PointOp sum;  // out0 = a * img0 + b * img1, alpha 1
        sum.w = w, sum.h = h;
        sum.body = "    out0 = vec4(img0(p).rgb * P[0] + img1(p).rgb * P[1], 1.0);\n";
        sum.full = true;
        Value glare;
        if (type == FogGlow) {
            const gpu::TexturePtr t = textureOf(hi);
            sum.params = {0.7f, 0.6f};
            glare = gpu::runPass(ctx, sum, {imageValue(gpu::boxBlur(t, size * 0.5f, size * 0.5f)),
                                            imageValue(gpu::boxBlur(t, size * 0.12f, size * 0.12f))},
                                 {true})[0];
        } else {
            // Streaks: shifted, faded copies at doubling offsets, as on the CPU.
            const int n = type == SimpleStar ? 4 : std::max(2, int(paramF(4)));
            const float angle0 = paramF(5) * kPi / 180.0f, fade = paramF(6);
            gpu::PointOp streak = gatherOp(hi, R"(
    vec2 c = vec2(p) + 0.5, d = vec2(P[0], P[1]);
    vec4 o = bilinear0(c - d, true) + bilinear0(c + d, true);
    out0 = vec4(fetch0(p).rgb + o.rgb * P[2], 1.0);
)", {});
            streak.full = true;
            streak.inlinable = false;
            const float share = 1.0f / (n * std::max(1.0f, std::log2(size)));
            for (int s = 0; s < n; ++s) {
                const float a = angle0 + (type == SimpleStar ? s * kPi / 4.0f : s * 2.0f * kPi / n);
                Value cur = hi;
                for (float step = 1.0f; step < size; step *= 2.0f) {
                    streak.params = {std::cos(a) * step, std::sin(a) * step, std::pow(fade, step)};
                    cur = gpu::runPass(ctx, streak, {cur}, {true})[0];
                }
                if (glare.empty()) {
                    sum.params = {share, 0.0f};
                    glare = gpu::runPass(ctx, sum, {cur, cur}, {true})[0];
                } else {
                    sum.params = {1.0f, share};
                    glare = gpu::runPass(ctx, sum, {glare, cur}, {true})[0];
                }
                gpu::materialize(glare);  // keeps the running sum one texture deep
            }
        }
        gpu::PointOp fin;
        fin.w = w, fin.h = h;
        fin.body = R"(
    vec3 g = img1(p).rgb * P[0];
    vec4 s = img0(p);
    out0 = vec4(clampColor(uLinear, s.rgb + g), s.a);
    out1 = vec4(clampColor(uLinear, g), 1.0);
)";
        fin.params = {strength};
        gpu::runPoint(ctx, *this, fin, {in[0], glare}, out);
    }
};

class SunBeamsNode : public Node {
public:
    NODELAB_NODE({"filter.sun_beams", "Sun Beams", "Filter",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Source X", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Source Y", 0.3f, 0.0f, 1.0f),
                   ParamDesc::Float("Length", 0.3f, 0.0f, 1.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float sx = paramF(0) * src->w, sy = paramF(1) * src->h, len = paramF(2);
        const int samples = 48;
        // Each pixel averages the image along the line toward the source: bright areas smear outward.
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float acc[3] = {0, 0, 0}, wsum = 0;
            float px = x + 0.5f, py = y + 0.5f;
            for (int i = 0; i < samples; ++i) {
                float t = len * float(i) / samples;
                float c[4];
                sampleBilinear(*src, px + (sx - px) * t, py + (sy - py) * t, c);
                float wgt = 1.0f - float(i) / samples;
                for (int k = 0; k < 3; ++k) acc[k] += c[k] * wgt;
                wsum += wgt;
            }
            for (int k = 0; k < 3; ++k) d[k] = acc[k] / wsum;
            d[3] = s[3];
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op = gatherOp(in[0], R"(
    vec2 src = vec2(P[0], P[1]) * vec2(size0), q = vec2(p) + 0.5;
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int i = 0; i < 48; ++i) {
        float t = P[2] * float(i) / 48.0, wt = 1.0 - float(i) / 48.0;
        acc += bilinear0(q + (src - q) * t, false).rgb * wt;
        wsum += wt;
    }
    out0 = vec4(acc / wsum, fetch0(p).a);
)", {paramF(0), paramF(1), paramF(2)});
        op.inlinable = false;
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

}  // namespace

void registerFilterNodes(NodeRegistry& r) {
    r.add<BlurNode>();
    r.add<DirectionalBlurNode>();
    r.add<BilateralBlurNode>();
    r.add<FilterNode>();
    r.add<DilateErodeNode>();
    r.add<KuwaharaNode>();
    r.add<PixelateNode>();
    r.add<PosterizeNode>();
    r.add<GlareNode>();
    r.add<SunBeamsNode>();
}
