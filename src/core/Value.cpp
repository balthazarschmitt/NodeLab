#include "core/Value.h"

#include <algorithm>

#include "core/Parallel.h"

const char* pinTypeName(PinType t) {
    switch (t) {
        case PinType::Image: return "Image";
        case PinType::Channel: return "Channel";
        case PinType::Number: return "Number";
    }
    return "?";
}

bool Value::empty() const {
    if (std::holds_alternative<std::monostate>(v)) return true;
    if (auto p = std::get_if<ImagePtr>(&v)) return !*p;
    if (auto p = std::get_if<ChannelPtr>(&v)) return !*p;
    return false;
}

bool Value::size(int& w, int& h) const {
    if (auto p = std::get_if<ImagePtr>(&v); p && *p && !(*p)->empty()) {
        w = (*p)->w;
        h = (*p)->h;
        return true;
    }
    if (auto p = std::get_if<ChannelPtr>(&v); p && *p && !(*p)->constant) {
        w = (*p)->w;
        h = (*p)->h;
        return true;
    }
    return false;
}

ChannelPtr toChannel(const Value& val) {
    if (auto p = std::get_if<ChannelPtr>(&val.v)) return *p;
    if (auto p = std::get_if<float>(&val.v)) return std::make_shared<Channel>(Channel::makeConstant(*p));
    if (auto p = std::get_if<ImagePtr>(&val.v); p && *p) {
        const Image& img = **p;
        auto out = std::make_shared<Channel>(Channel::makeSized(img.w, img.h));
        parallelFor(img.h, [&](int y) {
            for (int x = 0; x < img.w; ++x) {
                size_t i = static_cast<size_t>(y) * img.w + x;
                const float* s = img.pixel(i);
                out->data[i] = luminance(s[0], s[1], s[2]);
            }
        });
        return out;
    }
    return nullptr;
}

ImagePtr toImage(const Value& val, int w, int h) {
    if (auto p = std::get_if<ImagePtr>(&val.v)) return *p;
    ChannelPtr c = toChannel(val);
    if (!c) return nullptr;
    if (!c->constant) {
        w = c->w;
        h = c->h;
    }
    if (w <= 0 || h <= 0) w = h = 1;
    auto out = std::make_shared<Image>(w, h);
    parallelFor(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            size_t i = static_cast<size_t>(y) * w + x;
            float g = c->at(i);
            float* d = out->pixel(i);
            d[0] = d[1] = d[2] = g;
            d[3] = 1.0f;
        }
    });
    return out;
}

float toNumber(const Value& val, float fallback) {
    if (auto p = std::get_if<float>(&val.v)) return *p;
    if (auto p = std::get_if<ChannelPtr>(&val.v); p && *p && (*p)->constant) return (*p)->value;
    return fallback;
}

bool canConvert(PinType from, PinType to) {
    if (from == to) return true;
    switch (to) {
        case PinType::Image: return true;                       // anything can become an image
        case PinType::Channel: return true;                     // image->lum, number->broadcast
        case PinType::Number: return false;                     // only numbers are numbers
    }
    return false;
}

ImagePtr cropImage(const Image& src, int x0, int y0, int w, int h) {
    auto out = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y) {
        const float* s = src.pixel(size_t(y + y0) * src.w + x0);
        std::copy(s, s + size_t(w) * 4, out->pixel(size_t(y) * w));
    }
    return out;
}

Value cropValue(const Value& v, int x0, int y0, int w, int h) {
    int vw, vh;
    if (!v.size(vw, vh) || (x0 == 0 && y0 == 0 && w == vw && h == vh)) return v;
    if (auto p = std::get_if<ImagePtr>(&v.v)) return Value(cropImage(**p, x0, y0, w, h));
    const Channel& c = *std::get<ChannelPtr>(v.v);
    auto out = std::make_shared<Channel>(Channel::makeSized(w, h));
    for (int y = 0; y < h; ++y) {
        const float* s = c.data.data() + size_t(y + y0) * c.w + x0;
        std::copy(s, s + w, out->data.data() + size_t(y) * w);
    }
    return Value(ChannelPtr(out));
}

Value resampleValue(const Value& v, int w, int h) {
    int vw, vh;
    if (!v.size(vw, vh) || (vw == w && vh == h)) return v;
    // Nearest neighbour, as nodes sample inputs of another size (nodeutil::ImageSampler).
    auto sx = [&](int x) { return std::min(vw - 1, x * vw / std::max(1, w)); };
    auto sy = [&](int y) { return std::min(vh - 1, y * vh / std::max(1, h)); };
    if (auto p = std::get_if<ImagePtr>(&v.v)) {
        auto out = std::make_shared<Image>(w, h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const float* s = (*p)->pixel(size_t(sy(y)) * vw + sx(x));
                std::copy(s, s + 4, out->pixel(size_t(y) * w + x));
            }
        return Value(ImagePtr(out));
    }
    const Channel& c = *std::get<ChannelPtr>(v.v);
    auto out = std::make_shared<Channel>(Channel::makeSized(w, h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) out->data[size_t(y) * w + x] = c.data[size_t(sy(y)) * vw + sx(x)];
    return Value(ChannelPtr(out));
}
