// Transform / distortion nodes. All work by inverse mapping: for each output pixel, find where
// it comes from in the source and sample there (bilinear).
#include <cmath>

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
    NODELAB_NODE({"xform.crop", "Crop", "Transform",
                  {{"Image", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Left", 0.0f, 0.0f, 1.0f), ParamDesc::Float("Right", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Float("Top", 0.0f, 0.0f, 1.0f), ParamDesc::Float("Bottom", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Bool("Resize Image", true)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        int x0 = int(std::min(paramF(0), paramF(1)) * src->w), x1 = int(std::max(paramF(0), paramF(1)) * src->w);
        int y0 = int(std::min(paramF(2), paramF(3)) * src->h), y1 = int(std::max(paramF(2), paramF(3)) * src->h);
        x1 = std::max(x1, x0 + 1), y1 = std::max(y1, y0 + 1);
        x1 = std::min(x1, src->w), y1 = std::min(y1, src->h);
        if (paramB(4)) {
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

void registerTransformNodes(NodeRegistry& r) {
    r.add<TransformNode>();
    r.add<FlipNode>();
    r.add<CropNode>();
    r.add<LensDistortionNode>();
    r.add<DisplaceNode>();
    r.add<MapUVNode>();
    r.add<CornerPinNode>();
}
