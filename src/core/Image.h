#pragma once
#include <cstddef>
#include <memory>
#include <vector>

// Float RGBA image, interleaved, values nominally 0..1 (sRGB-encoded, not linear).
struct Image {
    int w = 0, h = 0;
    std::vector<float> px;

    Image() = default;
    Image(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_ * 4, 0.0f) {}

    size_t pixelCount() const { return static_cast<size_t>(w) * h; }
    bool empty() const { return w <= 0 || h <= 0; }
    float* pixel(size_t i) { return px.data() + i * 4; }
    const float* pixel(size_t i) const { return px.data() + i * 4; }
};

// Single float plane. A "constant" channel has no pixel data and represents a
// flat value at any resolution (used for Number -> Channel broadcast).
struct Channel {
    int w = 0, h = 0;
    bool constant = true;
    float value = 0.0f;
    std::vector<float> data;

    static Channel makeConstant(float v) {
        Channel c;
        c.value = v;
        return c;
    }
    static Channel makeSized(int w, int h) {
        Channel c;
        c.w = w;
        c.h = h;
        c.constant = false;
        c.data.assign(static_cast<size_t>(w) * h, 0.0f);
        return c;
    }

    float at(size_t i) const { return constant ? value : data[i]; }
    size_t pixelCount() const { return static_cast<size_t>(w) * h; }
};

using ImagePtr = std::shared_ptr<const Image>;
using ChannelPtr = std::shared_ptr<const Channel>;
