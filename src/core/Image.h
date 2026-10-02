#pragma once
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace parallel {
// Zero or copy n floats across the worker threads (never cancelled).
void zeroFill(float* p, size_t n);
void copyFloats(float* dst, const float* src, size_t n);
}  // namespace parallel

// An allocator whose vectors leave new elements uninitialised, so Image can zero and copy its
// pixels across threads instead: a 24 MP image is 384 MB, and std::vector's own fill runs on one
// core (about 60 ms there, against 15).
template <typename T>
struct UninitAllocator : std::allocator<T> {
    template <typename U>
    struct rebind {
        using other = UninitAllocator<U>;
    };
    UninitAllocator() = default;
    template <typename U>
    UninitAllocator(const UninitAllocator<U>&) noexcept {}
    template <typename U, typename... A>
    void construct(U* p, A&&... a) {
        if constexpr (sizeof...(A) == 0)
            ::new (static_cast<void*>(p)) U;
        else
            ::new (static_cast<void*>(p)) U(std::forward<A>(a)...);
    }
};

// Float RGBA image, interleaved. Values are sRGB-encoded 0..1 in legacy projects and unbounded
// scene-linear Rec.709 in scene-linear ones (see core/ColorManagement.h).
struct Image {
    int w = 0, h = 0;
    std::vector<float, UninitAllocator<float>> px;

    Image() = default;
    // Starts transparent black.
    Image(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_ * 4) { parallel::zeroFill(px.data(), px.size()); }
    Image(const Image& o) : w(o.w), h(o.h), px(o.px.size()) { parallel::copyFloats(px.data(), o.px.data(), px.size()); }
    Image(Image&&) = default;
    Image& operator=(const Image& o) {
        if (this != &o) {
            w = o.w, h = o.h;
            px.resize(o.px.size());
            parallel::copyFloats(px.data(), o.px.data(), px.size());
        }
        return *this;
    }
    Image& operator=(Image&&) = default;

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
