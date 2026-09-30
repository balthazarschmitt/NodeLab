#pragma once
#include <algorithm>
#include <memory>
#include <vector>

#include "core/Parallel.h"
#include "graph/Node.h"
#include "graph/NodeRegistry.h"

// Declares staticInfo()/info() for a node class from a NodeInfo initializer.
#define NODELAB_NODE(...)                                         \
    static const NodeInfo& staticInfo() {                         \
        static const NodeInfo inf = __VA_ARGS__;                  \
        return inf;                                               \
    }                                                             \
    const NodeInfo& info() const override { return staticInfo(); }

namespace nodeutil {

// Resolution of the first sized input, else the context default.
inline void resolveSize(const std::vector<Value>& in, const EvalContext& ctx, int& w, int& h) {
    for (const auto& v : in)
        if (v.size(w, h)) return;
    w = ctx.defaultW;
    h = ctx.defaultH;
}

// Channel for an input, or a constant `def` when nothing usable is there.
inline ChannelPtr channelOr(const Value& v, float def) {
    ChannelPtr c = toChannel(v);
    return c ? c : std::make_shared<Channel>(Channel::makeConstant(def));
}

// Samples a channel whose resolution may differ from the output (nearest neighbour).
// Values are clamped to [lo, hi]; parameter-driving channels use the parameter's declared range.
struct ChannelSampler {
    const Channel* c;
    int outW, outH;
    float lo = -1e30f, hi = 1e30f;
    float operator()(int x, int y) const {
        float v;
        if (c->constant) {
            v = c->value;
        } else if (c->w == outW && c->h == outH) {
            v = c->data[size_t(y) * outW + x];
        } else {
            int sx = std::min(c->w - 1, x * c->w / std::max(1, outW));
            int sy = std::min(c->h - 1, y * c->h / std::max(1, outH));
            v = c->data[size_t(sy) * c->w + sx];
        }
        return std::clamp(v, lo, hi);
    }
};

// Sampler for input pin `pin` of `node`, clamped to the range of the param backing that pin.
inline ChannelSampler paramSampler(const Node& node, int pin, const ChannelPtr& c, int w, int h) {
    ChannelSampler s{c.get(), w, h};
    int fp = node.info().inputs[pin].fallbackParam;
    if (fp >= 0) {
        s.lo = node.info().params[fp].hardMin;
        s.hi = node.info().params[fp].hardMax;
    }
    return s;
}

// New image of src's size; fn(x, y, srcPixel, dstPixel) fills each pixel (runs in parallel).
template <typename Fn>
std::shared_ptr<Image> mapImage(const Image& src, Fn&& fn) {
    auto out = std::make_shared<Image>(src.w, src.h);
    parallelFor(src.h, [&](int y) {
        for (int x = 0; x < src.w; ++x) {
            size_t i = size_t(y) * src.w + x;
            fn(x, y, src.pixel(i), out->pixel(i));
        }
    });
    return out;
}

// New channel of size w x h; fn(x, y) returns each value.
template <typename Fn>
std::shared_ptr<Channel> makeChannel(int w, int h, Fn&& fn) {
    auto out = std::make_shared<Channel>(Channel::makeSized(w, h));
    parallelFor(h, [&](int y) {
        for (int x = 0; x < w; ++x) out->data[size_t(y) * w + x] = fn(x, y);
    });
    return out;
}

// Same idea for images: returns a pointer to the RGBA of the nearest source pixel.
struct ImageSampler {
    const Image* img;
    int outW, outH;
    const float* operator()(int x, int y) const {
        if (img->w == outW && img->h == outH) return img->pixel(size_t(y) * outW + x);
        int sx = std::min(img->w - 1, x * img->w / std::max(1, outW));
        int sy = std::min(img->h - 1, y * img->h / std::max(1, outH));
        return img->pixel(size_t(sy) * img->w + sx);
    }
};

inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// Clamp for colour values: 0..1 for display-referred (legacy) projects; in scene-linear ones only
// negatives go, so highlights above 1 survive, as in Blender's colour nodes.
inline float clampColor(bool linear, float v) { return linear ? std::max(v, 0.0f) : clamp01(v); }

}  // namespace nodeutil
