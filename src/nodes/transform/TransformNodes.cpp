// Transform / distortion nodes. All work by inverse mapping: for each output pixel, find where
// it comes from in the source and sample there (bilinear).
#include "nodes/transform/TransformNodes.h"

#include <cmath>

#include "core/ColorMath.h"
#include "nodes/ImageOps.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;
using namespace imageops;

namespace {

constexpr float kPi = 3.14159265f;

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
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
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
        // Invert H (adjugate / determinant).
        double I[9] = {H[4] * H[8] - H[5] * H[7], H[2] * H[7] - H[1] * H[8], H[1] * H[5] - H[2] * H[4],
                       H[5] * H[6] - H[3] * H[8], H[0] * H[8] - H[2] * H[6], H[2] * H[3] - H[0] * H[5],
                       H[3] * H[7] - H[4] * H[6], H[1] * H[6] - H[0] * H[7], H[0] * H[4] - H[1] * H[3]};
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

void registerTransformNodes(NodeRegistry& r) {
    r.add<TransformNode>();
    r.add<FlipNode>();
    r.add<CropNode>();
    r.add<LensDistortionNode>();
    r.add<LensCorrectionNode>();
    r.add<DisplaceNode>();
    r.add<MapUVNode>();
    r.add<CornerPinNode>();
}
