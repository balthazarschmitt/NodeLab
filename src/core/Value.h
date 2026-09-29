#pragma once
#include <variant>

#include "core/Image.h"

enum class PinType { Image, Channel, Number };

const char* pinTypeName(PinType t);

// What travels along a wire. Empty (monostate) means "nothing connected / no data".
struct Value {
    std::variant<std::monostate, ImagePtr, ChannelPtr, float> v;

    Value() = default;
    Value(ImagePtr p) : v(std::move(p)) {}
    Value(ChannelPtr p) : v(std::move(p)) {}
    Value(float f) : v(f) {}

    bool empty() const;
    // Resolution carried by this value, or false for sizeless values (numbers, constant channels).
    bool size(int& w, int& h) const;
};

// Implicit conversions between wire types. Return null / nullopt-like values on empty input.
//   Image   -> Channel : Rec.709 luminance
//   Channel -> Image   : grayscale, alpha 1
//   Number  -> Channel : constant broadcast
//   Number/constant -> Image : flat gray at (w, h)
ChannelPtr toChannel(const Value& val);
ImagePtr toImage(const Value& val, int w, int h);
float toNumber(const Value& val, float fallback);

bool canConvert(PinType from, PinType to);

inline float luminance(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
