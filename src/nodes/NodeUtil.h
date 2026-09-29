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
struct ChannelSampler {
    const Channel* c;
    int outW, outH;
    float operator()(int x, int y) const {
        if (c->constant) return c->value;
        if (c->w == outW && c->h == outH) return c->data[size_t(y) * outW + x];
        int sx = std::min(c->w - 1, x * c->w / std::max(1, outW));
        int sy = std::min(c->h - 1, y * c->h / std::max(1, outH));
        return c->data[size_t(sy) * c->w + sx];
    }
};

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

}  // namespace nodeutil
