// Transform / distortion nodes. All work by inverse mapping: for each output pixel, find where
// it comes from in the source and sample there (bilinear).
#include "nodes/transform/TransformNodes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/ColorMath.h"
#include "gpu/Device.h"
#include "gpu/PointOp.h"
#include "nodes/ImageOps.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;
using namespace imageops;

namespace {

constexpr float kPi = 3.14159265f;

// GPU versions read their source anywhere (gather pin 0) and have its size, like mapImage.
gpu::PointOp gatherOp(const Value& src, std::string body, std::vector<float> params) {
    gpu::PointOp op;
    src.size(op.w, op.h);
    op.body = std::move(body);
    op.params = std::move(params);
    op.gather = {0};
    return op;
}

class TransformNode : public Node {
public:
    NODELAB_NODE({"xform.transform", "Transform", "Transform",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::FloatFree("X", 0.0f, -500.0f, 500.0f), ParamDesc::FloatFree("Y", 0.0f, -500.0f, 500.0f),
                   ParamDesc::FloatFree("Angle", 0.0f, -180.0f, 180.0f), ParamDesc::Float("Scale", 1.0f, 0.05f, 8.0f),
                   ParamDesc::Bool("Wrap", false)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float tx = paramF(0) * ctx.scale, ty = paramF(1) * ctx.scale, a = -paramF(2) * kPi / 180.0f;
        const float sc = std::max(paramF(3), 1e-3f);
        const bool wrap = paramB(4);
        const float cx = src->w * 0.5f, cy = src->h * 0.5f, ca = std::cos(a), sa = std::sin(a);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float*, float* d) {
            float px = x + 0.5f - cx - tx, py = y + 0.5f - cy - ty;
            float sx = (px * ca - py * sa) / sc + cx, sy = (px * sa + py * ca) / sc + cy;
            if (wrap) {
                sx = std::fmod(std::fmod(sx, float(src->w)) + src->w, float(src->w));
                sy = std::fmod(std::fmod(sy, float(src->h)) + src->h, float(src->h));
            }
            sampleBilinear(*src, sx, sy, d, !wrap);
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        in[0].size(w, h);
        const float a = -paramF(2) * kPi / 180.0f;
        gpu::runPoint(ctx, *this,
                      gatherOp(in[0], R"(
    vec2 c = vec2(size0) * 0.5;
    vec2 d = vec2(p) + 0.5 - c - vec2(P[0], P[1]);
    vec2 s = vec2(d.x * P[2] - d.y * P[3], d.x * P[3] + d.y * P[2]) / P[4] + c;
    if (P[5] != 0.0) s = mod(s, vec2(size0));
    out0 = bilinear0(s, P[5] == 0.0);
)",
                               {paramF(0) * ctx.scale, paramF(1) * ctx.scale, std::cos(a), std::sin(a), std::max(paramF(3), 1e-3f),
                                paramB(4) ? 1.0f : 0.0f}),
                      in, out);
    }
};

// Moves and zooms the picture inside the same frame, for framing by eye: X and Y are fractions
// of the image's width and height (so the preview matches the export), Zoom scales about the
// frame's centre, and wherever the moved picture doesn't reach is fully transparent. Nearest
// keeps hard pixels when zooming far in.
class PanZoomNode : public Node {
public:
    NODELAB_NODE({panzoom::kType, "Pan and Zoom", "Transform",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::FloatFree("Zoom", 1.0f, 0.1f, 8.0f), ParamDesc::FloatFree("X", 0.0f, -1.0f, 1.0f),
                   ParamDesc::FloatFree("Y", 0.0f, -1.0f, 1.0f), ParamDesc::Enum("Interpolation", 0, {"Bilinear", "Nearest"})}})
    // The zoom, kept to a sane range (the param is free so it can be typed past the slider), and
    // the offset in pixels of a w x h image.
    float zoom() const {
        const float z = paramF(panzoom::Zoom);
        return std::isfinite(z) ? std::clamp(z, 1e-3f, 1e4f) : 1.0f;
    }
    void offset(int w, int h, float& ox, float& oy) const {
        auto finite = [](float v) { return std::isfinite(v) ? std::clamp(v, -1e4f, 1e4f) : 0.0f; };
        ox = finite(paramF(panzoom::X)) * w;
        oy = finite(paramF(panzoom::Y)) * h;
    }
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float z = zoom(), cx = src->w * 0.5f, cy = src->h * 0.5f;
        float ox, oy;
        offset(src->w, src->h, ox, oy);
        const bool nearest = paramI(panzoom::Interpolation) == 1;
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float*, float* d) {
            const float sx = (x + 0.5f - cx - ox) / z + cx, sy = (y + 0.5f - cy - oy) / z + cy;
            if (!nearest) {
                sampleBilinear(*src, sx, sy, d, true);
                return;
            }
            // Compared as floats first, so a far-off coordinate never overflows the int.
            if (!(sx >= 0 && sy >= 0 && sx < src->w && sy < src->h)) {
                d[0] = d[1] = d[2] = d[3] = 0;
                return;
            }
            const int ix = std::min(int(sx), src->w - 1), iy = std::min(int(sy), src->h - 1);
            const float* p = src->pixel(size_t(iy) * src->w + ix);
            std::copy(p, p + 4, d);
        })));
    }

    // Nearest snaps at pixel boundaries, where the GPU's coordinates can round the other way.
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override {
        return paramI(panzoom::Interpolation) == 0 && gpu::sizedValue(in[0]);
    }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        in[0].size(w, h);
        float ox, oy;
        offset(w, h, ox, oy);
        gpu::runPoint(ctx, *this, gatherOp(in[0], R"(
    vec2 c = vec2(size0) * 0.5;
    vec2 s = (vec2(p) + 0.5 - c - vec2(P[0], P[1])) / P[2] + c;
    out0 = bilinear0(s, true);
)", {ox, oy, zoom()}), in, out);
    }
};

class FlipNode : public Node {
public:
    NODELAB_NODE({"xform.flip", "Flip", "Transform",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Enum("Axis", 0, {"Horizontal", "Vertical", "Both"})}})
    // A region reads the mirrored region, which flipped is exactly the output.
    bool roiMap(const EvalContext&, int inW, int inH, const PixelRect& o, PixelRect& in) const override {
        const int axis = paramI(0);
        in = o;
        if (axis != 1) in.x = inW - o.x - o.w;
        if (axis != 0) in.y = inH - o.y - o.h;
        return true;
    }
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const int axis = paramI(0);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float*, float* d) {
            int sx = axis != 1 ? src->w - 1 - x : x, sy = axis != 0 ? src->h - 1 - y : y;
            const float* p = src->pixel(size_t(sy) * src->w + sx);
            std::copy(p, p + 4, d);
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        const int axis = paramI(0);
        gpu::runPoint(ctx, *this, gatherOp(in[0], R"(
    ivec2 q = p;
    if (P[0] != 1.0) q.x = size0.x - 1 - p.x;
    if (P[0] != 0.0) q.y = size0.y - 1 - p.y;
    out0 = fetch0(q);
)", {float(axis)}), in, out);
    }
};

class CropNode : public Node {
public:
    NODELAB_NODE({crop::kType, "Crop", "Transform",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Left", 0.0f, 0.0f, 1.0f), ParamDesc::Float("Right", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Top", 0.0f, 0.0f, 1.0f), ParamDesc::Float("Bottom", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Bool("Resize Image", true), ParamDesc::Float("Angle", 0.0f, -45.0f, 45.0f),
                   ParamDesc::Enum("Aspect", 0, {"Free", "Original", "1:1", "4:5", "5:4", "2:3", "3:2", "3:4", "4:3",
                                                 "5:7", "7:5", "9:16", "16:9"}),
                   ParamDesc::Bool("Constrain to Image", true)}})
    // Straighten: the rotation around the centre and the scale that hides empty corners.
    void straighten(int w, int h, float& ca, float& sa, float& cover) const {
        const float a = -paramF(crop::Angle) * kPi / 180.0f;
        ca = std::cos(a);
        sa = std::sin(a);
        cover = paramB(crop::Constrain) ? std::max((w * std::fabs(ca) + h * std::fabs(sa)) / w,
                                                   (w * std::fabs(sa) + h * std::fabs(ca)) / h)
                                        : 1.0f;
    }
    // The kept box in pixels of an image of w x h.
    void box(int w, int h, int& x0, int& y0, int& x1, int& y1) const {
        const crop::Rect rc = crop::effectiveRect(*this, w, h);
        x0 = int(rc.l * w), x1 = int(rc.r * w);
        y0 = int(rc.t * h), y1 = int(rc.b * h);
        x1 = std::max(x1, x0 + 1), y1 = std::max(y1, y0 + 1);
        x1 = std::min(x1, w), y1 = std::min(y1, h);
        x0 = std::min(x0, x1 - 1), y0 = std::min(y0, y1 - 1);
    }
    void roiOutputSize(int inW, int inH, int& w, int& h) const override {
        w = inW;
        h = inH;
        if (!paramB(crop::ResizeImage)) return;
        int x0, y0, x1, y1;
        box(inW, inH, x0, y0, x1, y1);
        w = x1 - x0;
        h = y1 - y0;
    }
    // A region reads the matching part of the input: shifted by the box, and through the
    // straightening rotation (plus a pixel for bilinear sampling).
    bool roiMap(const EvalContext&, int inW, int inH, const PixelRect& o, PixelRect& in) const override {
        int x0 = 0, y0 = 0, x1, y1;
        if (paramB(crop::ResizeImage)) box(inW, inH, x0, y0, x1, y1);
        in = {o.x + x0, o.y + y0, o.w, o.h};
        if (paramF(crop::Angle) == 0.0f) return true;
        float ca, sa, cover;
        straighten(inW, inH, ca, sa, cover);
        const float cx = inW * 0.5f, cy = inH * 0.5f;
        float lx = 1e30f, ly = 1e30f, hx = -1e30f, hy = -1e30f;
        for (int c = 0; c < 4; ++c) {
            const float px = (float(c & 1 ? in.x + in.w : in.x) - cx) / cover;
            const float py = (float(c & 2 ? in.y + in.h : in.y) - cy) / cover;
            const float sx = px * ca - py * sa + cx, sy = px * sa + py * ca + cy;
            lx = std::min(lx, sx), hx = std::max(hx, sx);
            ly = std::min(ly, sy), hy = std::max(hy, sy);
        }
        const int ix0 = int(std::floor(lx)) - 2, iy0 = int(std::floor(ly)) - 2;
        in = {ix0, iy0, int(std::ceil(hx)) + 2 - ix0, int(std::ceil(hy)) + 2 - iy0};
        return true;
    }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        if (ctx.roi) {
            out[0] = Value(evaluateRegion(*ctx.roi, *src));
            return;
        }
        // Straighten first: rotate around the centre (positive = clockwise, like Lightroom), scaled up
        // just enough that no empty corners show when Constrain to Image is on.
        const float deg = paramF(crop::Angle);
        if (deg != 0.0f) {
            const float a = -deg * kPi / 180.0f, ca = std::cos(a), sa = std::sin(a);
            const float w = float(src->w), h = float(src->h);
            const float cover = paramB(crop::Constrain)
                                    ? std::max((w * std::fabs(ca) + h * std::fabs(sa)) / w, (w * std::fabs(sa) + h * std::fabs(ca)) / h)
                                    : 1.0f;
            const float cx = w * 0.5f, cy = h * 0.5f;
            ImagePtr rotated = mapImage(*src, [&](int x, int y, const float*, float* d) {
                const float px = (x + 0.5f - cx) / cover, py = (y + 0.5f - cy) / cover;
                sampleBilinear(*src, px * ca - py * sa + cx, px * sa + py * ca + cy, d, true);
            });
            src = rotated;
        }
        const crop::Rect rc = crop::effectiveRect(*this, src->w, src->h);
        int x0 = int(rc.l * src->w), x1 = int(rc.r * src->w);
        int y0 = int(rc.t * src->h), y1 = int(rc.b * src->h);
        x1 = std::max(x1, x0 + 1), y1 = std::max(y1, y0 + 1);
        x1 = std::min(x1, src->w), y1 = std::min(y1, src->h);
        x0 = std::min(x0, x1 - 1), y0 = std::min(y0, y1 - 1);
        if (paramB(crop::ResizeImage)) {
            auto img = std::make_shared<Image>(x1 - x0, y1 - y0);
            parallelFor(img->h, [&](int y) {
                for (int x = 0; x < img->w; ++x) {
                    const float* p = src->pixel(size_t(y + y0) * src->w + x + x0);
                    std::copy(p, p + 4, img->pixel(size_t(y) * img->w + x));
                }
            });
            out[0] = Value(ImagePtr(img));
        } else {
            out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
                bool in = x >= x0 && x < x1 && y >= y0 && y < y1;
                for (int k = 0; k < 4; ++k) d[k] = in ? s[k] : 0.0f;
            })));
        }
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        // As evaluateRegion: each output pixel's place in the straightened input, then the box. A
        // region (ctx.roi) is a window of the output reading a window of the input.
        int w, h;
        in[0].size(w, h);
        int rx = 0, ry = 0, ix = 0, iy = 0;  // output window origin, input buffer origin
        if (ctx.roi) {
            w = ctx.roi->inputW, h = ctx.roi->inputH;
            rx = ctx.roi->rect.x, ry = ctx.roi->rect.y;
            ix = ctx.roi->input.x, iy = ctx.roi->input.y;
        }
        int x0, y0, x1, y1;
        box(w, h, x0, y0, x1, y1);
        const bool resize = paramB(crop::ResizeImage);
        float ca = 1, sa = 0, cover = 1;
        const bool rotate = paramF(crop::Angle) != 0.0f;
        if (rotate) straighten(w, h, ca, sa, cover);
        gpu::PointOp op = gatherOp(in[0], R"(
    ivec2 q = p + ivec2(P[4], P[5]);
    if (q.x < int(P[0]) || q.x >= int(P[2]) || q.y < int(P[1]) || q.y >= int(P[3])) {
        out0 = vec4(0.0);
    } else if (P[6] != 0.0) {
        vec2 c = vec2(P[10], P[11]);
        vec2 d = (vec2(q) + 0.5 - c) / P[9];
        out0 = bilinear0(vec2(d.x * P[7] - d.y * P[8], d.x * P[8] + d.y * P[7]) + c - vec2(P[12], P[13]), true);
    } else {
        out0 = fetch0(q - ivec2(P[12], P[13]));
    }
)",
                                   {float(x0), float(y0), float(x1), float(y1), float(rx + (resize ? x0 : 0)),
                                    float(ry + (resize ? y0 : 0)), rotate ? 1.0f : 0.0f, ca, sa, cover, w * 0.5f, h * 0.5f,
                                    float(ix), float(iy)});
        if (ctx.roi) op.w = ctx.roi->rect.w, op.h = ctx.roi->rect.h;
        else if (resize) op.w = x1 - x0, op.h = y1 - y0;
        gpu::runPoint(ctx, *this, op, in, out);
    }

private:
    // The output region from the part of the input roiMap asked for (r.input): the same pixels as
    // the whole-image path above computes there.
    ImagePtr evaluateRegion(const RoiWindow& r, const Image& src) const {
        const int inW = r.inputW, inH = r.inputH;
        int x0, y0, x1, y1;
        box(inW, inH, x0, y0, x1, y1);
        const bool resize = paramB(crop::ResizeImage);
        const int ox = resize ? x0 : 0, oy = resize ? y0 : 0;
        const bool rotate = paramF(crop::Angle) != 0.0f;
        float ca = 1, sa = 0, cover = 1;
        if (rotate) straighten(inW, inH, ca, sa, cover);
        const float cx = inW * 0.5f, cy = inH * 0.5f;
        auto img = std::make_shared<Image>(r.rect.w, r.rect.h);
        parallelFor(img->h, [&](int by) {
            for (int bx = 0; bx < img->w; ++bx) {
                // Pixel in the straightened input.
                const int x = bx + r.rect.x + ox, y = by + r.rect.y + oy;
                float* d = img->pixel(size_t(by) * img->w + bx);
                if (!(x >= x0 && x < x1 && y >= y0 && y < y1)) {
                    std::fill(d, d + 4, 0.0f);
                } else if (rotate) {
                    const float px = (x + 0.5f - cx) / cover, py = (y + 0.5f - cy) / cover;
                    sampleBilinear(src, px * ca - py * sa + cx - r.input.x, px * sa + py * ca + cy - r.input.y, d, true);
                } else {
                    const float* p = src.pixel(size_t(y - r.input.y) * src.w + x - r.input.x);
                    std::copy(p, p + 4, d);
                }
            }
        });
        return img;
    }
};

class LensDistortionNode : public Node {
public:
    NODELAB_NODE({"xform.lens_distortion", "Lens Distortion", "Transform",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Distortion", 0.1f, -0.99f, 1.0f), ParamDesc::Float("Dispersion", 0.02f, 0.0f, 1.0f),
                   ParamDesc::Bool("Fit", true)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const float k = paramF(0), disp = paramF(1);
        const float half = std::max(src->w, src->h) * 0.5f;
        const float cx = src->w * 0.5f, cy = src->h * 0.5f;
        // Radial model: source = p * (1 + k_c * r^2), with k shifted per channel for dispersion
        // (red bends least, blue most, like a real lens).
        const float kc[3] = {k - disp * 0.5f, k, k + disp * 0.5f};
        const float rmax2 = (cx * cx + cy * cy) / (half * half);
        const float fit = paramB(2) && k > 0 ? 1.0f / (1.0f + (k + disp * 0.5f) * rmax2) : 1.0f;
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float px = (x + 0.5f - cx) / half * fit, py = (y + 0.5f - cy) / half * fit;
            float r2 = px * px + py * py;
            for (int c = 0; c < 3; ++c) {
                float m = 1.0f + kc[c] * r2;
                float smp[4];
                sampleBilinear(*src, px * m * half + cx, py * m * half + cy, smp, true);
                d[c] = smp[c];
            }
            d[3] = s[3];
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        in[0].size(w, h);
        const float k = paramF(0), disp = paramF(1);
        const float half = std::max(w, h) * 0.5f, cx = w * 0.5f, cy = h * 0.5f;
        const float rmax2 = (cx * cx + cy * cy) / (half * half);
        const float fit = paramB(2) && k > 0 ? 1.0f / (1.0f + (k + disp * 0.5f) * rmax2) : 1.0f;
        gpu::runPoint(ctx, *this, gatherOp(in[0], R"(
    vec2 c = vec2(size0) * 0.5;
    vec2 q = (vec2(p) + 0.5 - c) / P[3] * P[4];
    float r2 = q.x * q.x + q.y * q.y;
    for (int k = 0; k < 3; ++k) out0[k] = bilinear0(q * (1.0 + P[k] * r2) * P[3] + c, true)[k];
    out0.a = fetch0(p).a;
)", {k - disp * 0.5f, k, k + disp * 0.5f, half, fit}), in, out);
    }
};

// Lightroom's manual lens corrections: distortion, chromatic aberration fringes and vignetting.
class LensCorrectionNode : public Node {
public:
    NODELAB_NODE({"xform.lens_correction", "Lens Correction", "Transform",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Distortion", 0.0f, -100.0f, 100.0f), ParamDesc::Bool("Constrain to Image", true),
                   ParamDesc::Float("Red / Cyan", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Blue / Yellow", 0.0f, -100.0f, 100.0f),
                   ParamDesc::Float("Vignetting", 0.0f, -100.0f, 100.0f), ParamDesc::Float("Midpoint", 50.0f, 0.0f, 100.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        // Radii are relative to the half diagonal, so the corners are at r = 1.
        const float cx = src->w * 0.5f, cy = src->h * 0.5f, half = std::hypot(cx, cy);
        // Positive Distortion straightens barrel distortion (lines bowing outward) by pulling
        // the edges out; negative fixes pincushion. source = p * (1 + k r^2).
        const float k = -paramF(0) / 100.0f * 0.25f;
        const float zoom = paramB(1) && k > 0 ? 1.0f / (1.0f + k) : 1.0f;  // hide the empty corners
        // Fringe sliders scale the red and blue channels radially against green.
        const float sc[3] = {1.0f + paramF(2) / 100.0f * 0.005f, 1.0f, 1.0f + paramF(3) / 100.0f * 0.005f};
        const float vig = paramF(4) / 100.0f * 1.5f;  // stops at the corners
        const float power = 1.0f + 5.0f * paramF(5) / 100.0f;
        const bool geometry = k != 0.0f || sc[0] != 1.0f || sc[2] != 1.0f;
        const bool lin = ctx.linear();
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            const float px = (x + 0.5f - cx) / half, py = (y + 0.5f - cy) / half;
            const float r2 = px * px + py * py;
            if (geometry) {
                const float m = (1.0f + k * r2) * zoom;
                for (int c = 0; c < 3; ++c) {
                    float smp[4];
                    const float f = m * sc[c] * half;
                    sampleBilinear(*src, px * f + cx, py * f + cy, smp, !paramB(1));
                    d[c] = smp[c];
                    if (c == 1) d[3] = smp[3];
                }
            } else {
                std::copy(s, s + 4, d);
            }
            if (vig != 0.0f) {
                // Brighten (or darken) toward the corners in linear light, like a lens' falloff.
                const float g = std::exp2(vig * std::pow(std::min(r2, 1.0f), power * 0.5f));
                for (int c = 0; c < 3; ++c)
                    d[c] = lin ? d[c] * g : colormath::linearToSrgb(colormath::srgbToLinear(clamp01(d[c])) * g);
            }
            for (int c = 0; c < 3; ++c) d[c] = clampColor(lin, d[c]);
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        in[0].size(w, h);
        const float half = std::hypot(w * 0.5f, h * 0.5f);
        const float k = -paramF(0) / 100.0f * 0.25f;
        const float zoom = paramB(1) && k > 0 ? 1.0f / (1.0f + k) : 1.0f;
        const float sr = 1.0f + paramF(2) / 100.0f * 0.005f, sb = 1.0f + paramF(3) / 100.0f * 0.005f;
        const float vig = paramF(4) / 100.0f * 1.5f, power = 1.0f + 5.0f * paramF(5) / 100.0f;
        const bool geometry = k != 0.0f || sr != 1.0f || sb != 1.0f;
        gpu::runPoint(ctx, *this, gatherOp(in[0], R"(
    vec2 c = vec2(size0) * 0.5;
    vec2 q = (vec2(p) + 0.5 - c) / P[0];
    float r2 = q.x * q.x + q.y * q.y;
    vec4 d = fetch0(p);
    if (P[7] != 0.0) {
        float m = (1.0 + P[1] * r2) * P[2];
        for (int k = 0; k < 3; ++k) {
            vec4 smp = bilinear0(q * (m * P[3 + k] * P[0]) + c, P[8] == 0.0);
            d[k] = smp[k];
            if (k == 1) d.a = smp.a;
        }
    }
    if (P[6] != 0.0) {
        float g = exp2(P[6] * powPos(min(r2, 1.0), P[9] * 0.5));
        d.rgb = uLinear ? d.rgb * g : linearToSrgb(srgbToLinear(clamp01(d.rgb)) * g);
    }
    out0 = vec4(clampColor(uLinear, d.rgb), d.a);
)",
                                           {half, k, zoom, sr, 1.0f, sb, vig, geometry ? 1.0f : 0.0f, paramB(1) ? 1.0f : 0.0f, power}),
                      in, out);
    }
};

class DisplaceNode : public Node {
public:
    NODELAB_NODE({"xform.displace", "Displace", "Transform",
                  {{"Image", PinType::Image}, {"X", PinType::Channel}, {"Y", PinType::Channel}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::FloatFree("Strength X", 30.0f, -200.0f, 200.0f),
                   ParamDesc::FloatFree("Strength Y", 30.0f, -200.0f, 200.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        // 0.5 in the displacement channels means "no movement".
        ChannelPtr cx = channelOr(in[1], 0.5f), cy = channelOr(in[2], 0.5f);
        ChannelSampler sx{cx.get(), src->w, src->h}, sy{cy.get(), src->w, src->h};
        const float kx = paramF(0) * ctx.scale * 2.0f, ky = paramF(1) * ctx.scale * 2.0f;
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float*, float* d) {
            sampleBilinear(*src, x + 0.5f + (sx(x, y) - 0.5f) * kx, y + 0.5f + (sy(x, y) - 0.5f) * ky, d);
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op = gatherOp(in[0], R"(
    out0 = bilinear0(vec2(p) + 0.5 + (vec2(ch1(p), ch2(p)) - 0.5) * vec2(P[0], P[1]), false);
)", {paramF(0) * ctx.scale * 2.0f, paramF(1) * ctx.scale * 2.0f});
        op.defaults = {NAN, 0.5f, 0.5f};
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

class MapUVNode : public Node {
public:
    NODELAB_NODE({"xform.map_uv", "Map UV", "Transform",
                  {{"Image", PinType::Image}, {"UV", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        ImagePtr uv = toImage(in[1], 0, 0);
        if (!src) return;
        if (!uv) {
            out[0] = Value(src);
            return;
        }
        // UV image: red = u (0..1 across), green = v (0..1 down); output has the UV image's size.
        out[0] = Value(ImagePtr(mapImage(*uv, [&](int, int, const float* t, float* d) {
            sampleBilinear(*src, t[0] * src->w, t[1] * src->h, d, true);
        })));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override {
        return gpu::sizedValue(in[0]) && gpu::sizedValue(in[1]);
    }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        gpu::PointOp op = gatherOp(in[0], R"(
    vec4 t = img1(p);
    out0 = bilinear0(t.xy * vec2(size0), true);
)", {});
        in[1].size(op.w, op.h);  // the UV image's size
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

class CornerPinNode : public Node {
public:
    NODELAB_NODE({"xform.corner_pin", "Corner Pin", "Transform",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::FloatFree("Upper Left X", 0.0f, 0.0f, 1.0f), ParamDesc::FloatFree("Upper Left Y", 0.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("Upper Right X", 1.0f, 0.0f, 1.0f), ParamDesc::FloatFree("Upper Right Y", 0.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("Lower Right X", 1.0f, 0.0f, 1.0f), ParamDesc::FloatFree("Lower Right Y", 1.0f, 0.0f, 1.0f),
                   ParamDesc::FloatFree("Lower Left X", 0.0f, 0.0f, 1.0f), ParamDesc::FloatFree("Lower Left Y", 1.0f, 0.0f, 1.0f)}})
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override { return gpu::sizedValue(in[0]); }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        double I[9];
        inverse(I);
        std::vector<float> m(I, I + 9);
        gpu::runPoint(ctx, *this, gatherOp(in[0], R"(
    vec2 uv = (vec2(p) + 0.5) / vec2(size0);
    float w = P[6] * uv.x + P[7] * uv.y + P[8];
    if (abs(w) < 1e-12) { out0 = vec4(0.0); return; }
    vec2 st = vec2(P[0] * uv.x + P[1] * uv.y + P[2], P[3] * uv.x + P[4] * uv.y + P[5]) / w;
    if (st.x < 0.0 || st.x > 1.0 || st.y < 0.0 || st.y > 1.0) { out0 = vec4(0.0); return; }
    out0 = bilinear0(st * vec2(size0), false);
)", m), in, out);
    }

    // The homography from output to source coordinates (both 0..1).
    void inverse(double I[9]) const {
        // Homography mapping the unit square to the quad (Heckbert's square-to-quad), then inverted.
        double x0 = paramF(0), y0 = paramF(1), x1 = paramF(2), y1 = paramF(3);
        double x2 = paramF(4), y2 = paramF(5), x3 = paramF(6), y3 = paramF(7);
        double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
        double H[9];
        if (std::fabs(sx) < 1e-12 && std::fabs(sy) < 1e-12) {
            H[0] = x1 - x0, H[1] = x2 - x1, H[2] = x0, H[3] = y1 - y0, H[4] = y2 - y1, H[5] = y0, H[6] = 0, H[7] = 0, H[8] = 1;
        } else {
            double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
            double den = dx1 * dy2 - dx2 * dy1;
            if (std::fabs(den) < 1e-12) den = 1e-12;
            double g = (sx * dy2 - dx2 * sy) / den, h = (dx1 * sy - sx * dy1) / den;
            H[0] = x1 - x0 + g * x1, H[1] = x3 - x0 + h * x3, H[2] = x0;
            H[3] = y1 - y0 + g * y1, H[4] = y3 - y0 + h * y3, H[5] = y0;
            H[6] = g, H[7] = h, H[8] = 1;
        }
        // Invert H (adjugate; the scale cancels in the projective divide).
        const double A[9] = {H[4] * H[8] - H[5] * H[7], H[2] * H[7] - H[1] * H[8], H[1] * H[5] - H[2] * H[4],
                             H[5] * H[6] - H[3] * H[8], H[0] * H[8] - H[2] * H[6], H[2] * H[3] - H[0] * H[5],
                             H[3] * H[7] - H[4] * H[6], H[1] * H[6] - H[0] * H[7], H[0] * H[4] - H[1] * H[3]};
        std::copy(A, A + 9, I);
    }

    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        double I[9];
        inverse(I);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float*, float* d) {
            double u = (x + 0.5) / src->w, v = (y + 0.5) / src->h;
            double w = I[6] * u + I[7] * v + I[8];
            if (std::fabs(w) < 1e-12) {
                d[0] = d[1] = d[2] = d[3] = 0;
                return;
            }
            double s = (I[0] * u + I[1] * v + I[2]) / w, t = (I[3] * u + I[4] * v + I[5]) / w;
            if (s < 0 || s > 1 || t < 0 || t > 1) {
                d[0] = d[1] = d[2] = d[3] = 0;
                return;
            }
            sampleBilinear(*src, float(s * src->w), float(t * src->h), d);
        })));
    }
};

}  // namespace

namespace crop {

float aspectRatio(int option, int imageW, int imageH) {
    static const float kRatios[] = {0.0f, 0.0f, 1.0f, 4.0f / 5, 5.0f / 4, 2.0f / 3, 3.0f / 2, 3.0f / 4, 4.0f / 3,
                                    5.0f / 7, 7.0f / 5, 9.0f / 16, 16.0f / 9};
    if (option == 1) return imageH > 0 ? float(imageW) / imageH : 0.0f;
    if (option < 0 || option >= int(std::size(kRatios))) return 0.0f;
    return kRatios[option];
}

Rect effectiveRect(const Node& n, int imageW, int imageH) {
    Rect rc{std::min(n.paramF(Left), n.paramF(Right)), std::max(n.paramF(Left), n.paramF(Right)),
            std::min(n.paramF(Top), n.paramF(Bottom)), std::max(n.paramF(Top), n.paramF(Bottom))};
    const float ar = n.params.size() > size_t(Aspect) ? aspectRatio(n.paramI(Aspect), imageW, imageH) : 0.0f;
    if (ar <= 0.0f || imageW <= 0 || imageH <= 0) return rc;
    // Shrink the longer side around the centre until width / height (in pixels) matches.
    const float wpx = (rc.r - rc.l) * imageW, hpx = (rc.b - rc.t) * imageH;
    if (wpx <= 0 || hpx <= 0) return rc;
    if (wpx / hpx > ar) {
        const float cxr = (rc.l + rc.r) * 0.5f, hwf = hpx * ar / imageW * 0.5f;
        rc.l = cxr - hwf, rc.r = cxr + hwf;
    } else {
        const float cyr = (rc.t + rc.b) * 0.5f, hhf = wpx / ar / imageH * 0.5f;
        rc.t = cyr - hhf, rc.b = cyr + hhf;
    }
    return rc;
}

}  // namespace crop

// ---------------------------------------------------------------- perspective

namespace perspective {

namespace {

constexpr double kPiD = 3.14159265358979323846;
// The Vertical and Horizontal sliders tilt the camera by up to this much at their ends.
constexpr double kMaxTilt = 30.0 * kPiD / 180.0;

Mat mul(const Mat& a, const Mat& b) {
    Mat r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    return r;
}

Mat diag(double x, double y) { return {x, 0, 0, 0, y, 0, 0, 0, 1}; }
Mat shift(double x, double y) { return {1, 0, x, 0, 1, y, 0, 0, 1}; }
Mat rotX(double a) { return {1, 0, 0, 0, std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a)}; }
Mat rotY(double a) { return {std::cos(a), 0, std::sin(a), 0, 1, 0, -std::sin(a), 0, std::cos(a)}; }
Mat rotZ(double a) { return {std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a), 0, 0, 0, 1}; }
Mat rotation(const double a[3]) { return mul(rotZ(a[2]), mul(rotY(a[1]), rotX(a[0]))); }
Mat transpose(const Mat& m) { return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]}; }

// Solves the 3x3 system A x = b (Cramer); false when it's singular.
bool solve3(const double A[9], const double b[3], double x[3]) {
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

bool Guide::vertical(float aspect) const { return std::fabs(y1 - y0) > std::fabs(x1 - x0) * aspect; }

bool apply(const Mat& m, double u, double v, double& su, double& sv) {
    const double w = m[6] * u + m[7] * v + m[8];
    if (!(w > 1e-9)) return false;
    su = (m[0] * u + m[1] * v + m[2]) / w;
    sv = (m[3] * u + m[4] * v + m[5]) / w;
    return std::isfinite(su) && std::isfinite(sv);
}

Mat inverse(const Mat& m) {
    const Mat a = {m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
                   m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
                   m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
    const double det = m[0] * a[0] + m[1] * a[3] + m[2] * a[6];
    if (!(std::fabs(det) > 1e-300)) return diag(1, 1);
    Mat r;
    for (int i = 0; i < 9; ++i) r[i] = a[i] / det;  // divided, so points in front keep w > 0
    return r;
}

}  // namespace perspective

void PerspectiveNode::guidedAngles(int w, int h, double ang[3]) const {
    using namespace perspective;
    ang[0] = ang[1] = ang[2] = 0;
    if (paramI(Upright) != UprightGuided || guides.empty()) return;
    w = std::max(w, 1), h = std::max(h, 1);
    const double cx = w * 0.5, cy = h * 0.5, f = std::hypot(cx, cy);
    std::vector<UprightLine> lines;
    for (const Guide& g : guides) {
        const UprightLine l{(g.x0 * w - cx) / f, (g.y0 * h - cy) / f, (g.x1 * w - cx) / f, (g.y1 * h - cy) / f,
                            g.vertical(float(w) / h), 1.0};
        if (std::hypot(l.x1 - l.x0, l.y1 - l.y0) > 1e-4) lines.push_back(l);
        if (int(lines.size()) == kMaxGuides) break;
    }
    const bool free[3] = {true, true, true};
    solveUpright(lines, free, ang);
}

void PerspectiveNode::detectedAngles(double ang[3]) const {
    std::lock_guard<std::mutex> lock(detectedMutex_);
    std::copy(detected_, detected_ + 3, ang);
}

void PerspectiveNode::uprightAngles(EvalContext& ctx, const Image* src, int w, int h, double ang[3]) {
    using namespace perspective;
    ang[0] = ang[1] = ang[2] = 0;
    const int mode = paramI(Upright);
    if (!uprightDetects(mode)) {
        guidedAngles(w, h, ang);
        return;
    }
    if (ctx.roi && ctx.previewStats && ctx.previewStats->size() == 3) {
        for (int k = 0; k < 3; ++k) ang[k] = (*ctx.previewStats)[size_t(k)];
    } else if (src) {
        autoUprightAngles(detectLines(*src, ctx.linear()), w, h, mode, ang);
    }
    if (ctx.statsOut) *ctx.statsOut = {float(ang[0]), float(ang[1]), float(ang[2])};
    // Kept for the viewer's overlay and the Inspector, which ask for the matrix without pixels.
    std::lock_guard<std::mutex> lock(detectedMutex_);
    std::copy(ang, ang + 3, detected_);
}

perspective::Mat PerspectiveNode::matrix(int w, int h, const double* upright) const {
    using namespace perspective;
    w = std::max(w, 1), h = std::max(h, 1);
    const double cx = w * 0.5, cy = h * 0.5, f = std::hypot(cx, cy);
    double g[3];
    if (upright)
        std::copy(upright, upright + 3, g);
    else if (uprightDetects(paramI(Upright)))
        detectedAngles(g);
    else
        guidedAngles(w, h, g);
    // The sliders turn the camera further, on top of what the guides asked for. Vertical below
    // zero tilts the top towards the viewer (widens it), as Lightroom's slider does for
    // buildings leaning back; Horizontal above zero widens the right side; Rotate turns clockwise.
    const Mat R = mul(rotZ(paramF(Rotate) * kPiD / 180.0),
                      mul(rotY(paramF(Horizontal) / 100.0 * kMaxTilt), mul(rotX(-paramF(Vertical) / 100.0 * kMaxTilt), rotation(g))));
    // Output to input, in pixels about the centre: undo the offset, scale, aspect, then the
    // camera rotation (the input's ray is R^T times the output's).
    const double ox = paramF(OffsetX) / 100.0 * cx, oy = paramF(OffsetY) / 100.0 * cy;  // Y Offset > 0 moves up
    const double sx = std::exp2(paramF(Aspect) / 200.0), sy = 1.0 / sx;
    const Mat E = mul(diag(f, f), mul(transpose(R), diag(1.0 / f, 1.0 / f)));
    const auto build = [&](double scale) {
        Mat m = mul(diag(1.0 / scale, 1.0 / scale), shift(-ox, oy));
        m = mul(E, mul(diag(1.0 / sx, 1.0 / sy), m));
        // From image-relative output coordinates, to image-relative input ones.
        return mul(diag(1.0 / w, 1.0 / h), mul(shift(cx, cy), mul(m, mul(shift(-cx, -cy), diag(w, h)))));
    };
    const double scale = paramF(Scale) / 100.0;
    if (!paramB(Constrain)) return build(scale);
    // Constrain Crop: zoom in until the output's edges all land inside the input (no empty
    // corners). Bisection on the zoom, checking points along the border.
    const auto inside = [&](double s) {
        const Mat m = build(s);
        constexpr int kSteps = 16;
        for (int i = 0; i <= kSteps; ++i) {
            const double t = double(i) / kSteps;
            const double pts[4][2] = {{t, 0}, {t, 1}, {0, t}, {1, t}};
            for (const auto& p : pts) {
                double u, v;
                if (!apply(m, p[0], p[1], u, v) || u < -1e-6 || u > 1 + 1e-6 || v < -1e-6 || v > 1 + 1e-6) return false;
            }
        }
        return true;
    };
    if (inside(scale)) return build(scale);
    double lo = scale, hi = scale * 2;
    while (!inside(hi) && hi < scale * 16) lo = hi, hi *= 2;
    if (!inside(hi)) return build(scale);  // can't be filled (rotated out of view)
    for (int i = 0; i < 40; ++i) {
        const double mid = (lo + hi) * 0.5;
        (inside(mid) ? hi : lo) = mid;
    }
    return build(hi);
}

void PerspectiveNode::evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) {
    ImagePtr src = toImage(in[0], 0, 0);
    if (!src) return;
    double up[3];
    uprightAngles(ctx, src.get(), src->w, src->h, up);
    const perspective::Mat M = matrix(src->w, src->h, up);
    if (M == perspective::Mat{1, 0, 0, 0, 1, 0, 0, 0, 1}) {
        out[0] = Value(src);
        return;
    }
    out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float*, float* d) {
        double su, sv;
        if (!perspective::apply(M, (x + 0.5) / src->w, (y + 0.5) / src->h, su, sv)) {
            d[0] = d[1] = d[2] = d[3] = 0;
            return;
        }
        // Clamped far outside, where floats can't hold the position.
        sampleBilinear(*src, float(std::clamp(su, -4.0, 5.0) * src->w), float(std::clamp(sv, -4.0, 5.0) * src->h), d, true);
    })));
}

bool PerspectiveNode::gpuSupported(const EvalContext&, const std::vector<Value>& in) const { return gpu::sizedValue(in[0]); }

void PerspectiveNode::evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) {
    int w, h;
    in[0].size(w, h);
    // The detecting modes look at the pixels on the CPU (only for the preview: regions reuse its
    // rotation).
    ImagePtr src;
    if (perspective::uprightDetects(paramI(perspective::Upright)) && !(ctx.roi && ctx.previewStats)) {
        if (auto gi = std::get_if<GpuImagePtr>(&in[0].v))
            src = gpu::download(**gi);
        else if (auto gc = std::get_if<GpuChannelPtr>(&in[0].v))
            src = toImage(Value(gpu::download(**gc)), 0, 0);
        else
            src = toImage(in[0], 0, 0);
    }
    double up[3];
    uprightAngles(ctx, src.get(), w, h, up);
    const perspective::Mat M = matrix(w, h, up);
    if (M == perspective::Mat{1, 0, 0, 0, 1, 0, 0, 0, 1}) {
        out[0] = in[0];
        return;
    }
    gpu::runPoint(ctx, *this, gatherOp(in[0], R"(
    vec2 uv = (vec2(p) + 0.5) / vec2(size0);
    float w = P[6] * uv.x + P[7] * uv.y + P[8];
    if (!(w > 1e-9)) { out0 = vec4(0.0); return; }
    vec2 st = vec2(P[0] * uv.x + P[1] * uv.y + P[2], P[3] * uv.x + P[4] * uv.y + P[5]) / w;
    out0 = bilinear0(clamp(st, -4.0, 5.0) * vec2(size0), true);
)", std::vector<float>(M.begin(), M.end())), in, out);
}

void PerspectiveNode::saveExtra(nlohmann::json& j) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const perspective::Guide& g : guides) arr.push_back({g.x0, g.y0, g.x1, g.y1});
    j["guides"] = arr;
}

void PerspectiveNode::loadExtra(const nlohmann::json& j) {
    guides.clear();
    auto it = j.find("guides");
    if (it == j.end() || !it->is_array()) return;
    for (const auto& g : *it) {
        if (!g.is_array() || g.size() != 4 || int(guides.size()) == perspective::kMaxGuides) continue;
        float v[4];
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            ok = ok && g[k].is_number();
            v[k] = ok ? g[k].get<float>() : 0.0f;
            ok = ok && std::isfinite(v[k]);
        }
        if (ok) guides.push_back({std::clamp(v[0], 0.0f, 1.0f), std::clamp(v[1], 0.0f, 1.0f), std::clamp(v[2], 0.0f, 1.0f),
                                  std::clamp(v[3], 0.0f, 1.0f)});
    }
}

std::string PerspectiveNode::signatureExtra() const {
    std::string sig = "guides:";
    char buf[96];
    for (const perspective::Guide& g : guides) {
        std::snprintf(buf, sizeof buf, "%a,%a,%a,%a;", g.x0, g.y0, g.x1, g.y1);
        sig += buf;
    }
    return sig;
}

// ---------------------------------------------------------------- Lens Profile

LensProfileNode::Sample LensProfileNode::sampleAt(float r) const {
    Sample s{{1, 1, 1}, 1};
    const lensdb::Profile& p = profile;
    const double ru = std::max(double(r) * p.distScale, 1e-6);
    const double rd = ru + paramF(Distortion) / 100.0 * (lensdb::distort(p, ru) - ru);
    const double mg = rd / ru;
    s.m[0] = s.m[1] = s.m[2] = float(mg);
    if (paramB(ChromaticAberration) && p.tcaModel != lensdb::Profile::TcaNone) {
        s.m[0] = float(mg * lensdb::tcaScale(p, 0, rd));
        s.m[2] = float(mg * lensdb::tcaScale(p, 2, rd));
    }
    // Vignetting was measured on the photo as it came from the lens: at the source radius.
    if (p.vig && paramF(Vignetting) > 0)
        s.gain = float(std::pow(lensdb::vignetteGain(p, double(r) * mg * p.vigScale), paramF(Vignetting) / 100.0));
    return s;
}

float LensProfileNode::zoom(int w, int h) const {
    if (!paramB(Constrain) || !profile.valid() || w <= 0 || h <= 0) return 1.0f;
    const float cx = w * 0.5f, cy = h * 0.5f, half = std::hypot(cx, cy);
    // Every channel of 16 points per edge must read inside the photo.
    const auto fits = [&](float z) {
        for (int i = 0; i <= 16; ++i) {
            const float t = i / 16.0f;
            const float pts[4][2] = {{t * w, 0}, {t * w, float(h)}, {0, t * h}, {float(w), t * h}};
            for (const auto& pt : pts) {
                const float qx = (pt[0] - cx) / half * z, qy = (pt[1] - cy) / half * z;
                const Sample s = sampleAt(std::hypot(qx, qy));
                for (float m : s.m) {
                    const float sx = cx + qx * m * half, sy = cy + qy * m * half;
                    if (sx < -0.01f || sx > w + 0.01f || sy < -0.01f || sy > h + 0.01f) return false;
                }
            }
        }
        return true;
    };
    if (fits(1.0f)) return 1.0f;
    float lo = 0.25f, hi = 1.0f;
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (lo + hi);
        (fits(mid) ? lo : hi) = mid;
    }
    return lo;
}

namespace {

bool lensProfileActive(const LensProfileNode& n) {
    const lensdb::Profile& p = n.profile;
    return (p.distModel != lensdb::Profile::DistNone && n.paramF(LensProfileNode::Distortion) > 0.0f) ||
           (p.tcaModel != lensdb::Profile::TcaNone && n.paramB(LensProfileNode::ChromaticAberration)) ||
           (p.vig && n.paramF(LensProfileNode::Vignetting) > 0.0f);
}

}  // namespace

void LensProfileNode::evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) {
    ImagePtr src = toImage(in[0], 0, 0);
    if (!src) return;
    if (!lensProfileActive(*this)) {
        out[0] = Value(src);
        return;
    }
    const float cx = src->w * 0.5f, cy = src->h * 0.5f, half = std::hypot(cx, cy);
    const float z = zoom(src->w, src->h);
    const bool transparent = !paramB(Constrain), lin = ctx.linear();
    out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float*, float* d) {
        const float qx = (x + 0.5f - cx) / half * z, qy = (y + 0.5f - cy) / half * z;
        const Sample s = sampleAt(std::hypot(qx, qy));
        float smp[4];
        sampleBilinear(*src, cx + qx * s.m[1] * half, cy + qy * s.m[1] * half, smp, transparent);
        std::copy(smp, smp + 4, d);
        for (int c : {0, 2})
            if (s.m[c] != s.m[1]) {
                sampleBilinear(*src, cx + qx * s.m[c] * half, cy + qy * s.m[c] * half, smp, transparent);
                d[c] = smp[c];
            }
        if (s.gain != 1.0f)
            for (int c = 0; c < 3; ++c)
                d[c] = lin ? d[c] * s.gain : colormath::linearToSrgb(colormath::srgbToLinear(clamp01(d[c])) * s.gain);
        for (int c = 0; c < 3; ++c) d[c] = clampColor(lin, d[c]);
    })));
}

bool LensProfileNode::gpuSupported(const EvalContext&, const std::vector<Value>& in) const { return gpu::sizedValue(in[0]); }

void LensProfileNode::evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) {
    if (!lensProfileActive(*this)) {
        out[0] = in[0];
        return;
    }
    int w, h;
    in[0].size(w, h);
    const lensdb::Profile& p = profile;
    const bool tca = paramB(ChromaticAberration) && p.tcaModel != lensdb::Profile::TcaNone;
    const bool vig = p.vig && paramF(Vignetting) > 0;
    gpu::runPoint(ctx, *this, gatherOp(in[0], R"(
    vec2 c = vec2(size0) * 0.5;
    vec2 q = (vec2(p) + 0.5 - c) / P[0] * P[1];
    float r = length(q);
    float ru = max(r * P[2], 1e-6);
    float r2 = ru * ru;
    float rdf = ru;
    if (P[4] == 1.0) rdf = ru * (1.0 - P[5] + P[5] * r2);
    else if (P[4] == 2.0) rdf = ru * (1.0 + P[5] * r2 + P[6] * r2 * r2);
    else if (P[4] == 3.0) rdf = ru * (P[5] * r2 * ru + P[6] * r2 + P[7] * ru + 1.0 - P[5] - P[6] - P[7]);
    float rd = ru + P[3] * (rdf - ru);
    float mg = rd / ru;
    vec3 m = vec3(mg);
    if (P[8] == 1.0) { m.r = mg * P[9]; m.b = mg * P[12]; }
    else if (P[8] == 2.0) { m.r = mg * (P[9] * rd * rd + P[10] * rd + P[11]); m.b = mg * (P[12] * rd * rd + P[13] * rd + P[14]); }
    bool tr = P[21] != 0.0;
    vec4 d = bilinear0(c + q * (m.g * P[0]), tr);
    if (m.r != m.g) d.r = bilinear0(c + q * (m.r * P[0]), tr).r;
    if (m.b != m.g) d.b = bilinear0(c + q * (m.b * P[0]), tr).b;
    if (P[15] != 0.0) {
        float rv = r * mg * P[19];
        float v2 = rv * rv;
        float gain = pow(1.0 / max(1.0 + P[16] * v2 + P[17] * v2 * v2 + P[18] * v2 * v2 * v2, 0.05), P[20]);
        d.rgb = uLinear ? d.rgb * gain : linearToSrgb(srgbToLinear(clamp01(d.rgb)) * gain);
    }
    out0 = vec4(clampColor(uLinear, d.rgb), d.a);
)",
                                       {std::hypot(w * 0.5f, h * 0.5f), zoom(w, h), p.distScale, paramF(Distortion) / 100.0f,
                                        float(p.distModel), p.dist[0], p.dist[1], p.dist[2], tca ? float(p.tcaModel) : 0.0f,
                                        p.tcaR[0], p.tcaR[1], p.tcaR[2], p.tcaB[0], p.tcaB[1], p.tcaB[2], vig ? 1.0f : 0.0f,
                                        p.vigK[0], p.vigK[1], p.vigK[2], p.vigScale, paramF(Vignetting) / 100.0f,
                                        paramB(Constrain) ? 0.0f : 1.0f}),
                  in, out);
}

void LensProfileNode::saveExtra(nlohmann::json& j) const {
    if (profile.valid()) j["profile"] = profile.toJson();
}

void LensProfileNode::loadExtra(const nlohmann::json& j) {
    profile = j.is_object() && j.contains("profile") ? lensdb::Profile::fromJson(j["profile"]) : lensdb::Profile{};
}

std::string LensProfileNode::signatureExtra() const { return profile.signature(); }

void registerTransformNodes(NodeRegistry& r) {
    r.add<TransformNode>();
    r.add<PanZoomNode>();
    r.add<FlipNode>();
    r.add<CropNode>();
    r.add<LensDistortionNode>();
    r.add<LensCorrectionNode>();
    r.add<DisplaceNode>();
    r.add<MapUVNode>();
    r.add<CornerPinNode>();
    r.add<PerspectiveNode>();
    r.add<LensProfileNode>();
}
