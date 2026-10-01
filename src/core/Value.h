#pragma once
#include <variant>

#include "core/Image.h"

enum class PinType { Image, Channel, Number };

namespace gpu {
class Texture;
struct Pending;
}

// An image (RGBA) or channel (one plane) resident on the GPU (gpu/Device.h). Only the evaluator
// and GPU kernels see these: CPU nodes get their inputs downloaded first, and the conversions
// below download too.
//
// A per-pixel node's result starts out pending (gpu/PointOp.h): its shader hasn't run, so the
// next per-pixel node can compile it into its own shader (fusion) instead of reading a texture.
// texture() runs it when something needs the pixels.
struct GpuValue {
    mutable std::shared_ptr<gpu::Texture> tex;  // null while pending
    mutable std::shared_ptr<gpu::Pending> pending;
    int pendingOut = 0;  // which of the pending op's outputs this is
    int w = 0, h = 0;
    // The pixels, computing them if pending. Needs the GPU device; throws gpu::Error.
    const std::shared_ptr<gpu::Texture>& texture() const;
};
struct GpuImage : GpuValue {};
struct GpuChannel : GpuValue {};
using GpuImagePtr = std::shared_ptr<const GpuImage>;
using GpuChannelPtr = std::shared_ptr<const GpuChannel>;

const char* pinTypeName(PinType t);

// What travels along a wire. Empty (monostate) means "nothing connected / no data".
struct Value {
    std::variant<std::monostate, ImagePtr, ChannelPtr, float, GpuImagePtr, GpuChannelPtr> v;

    Value() = default;
    Value(ImagePtr p) : v(std::move(p)) {}
    Value(ChannelPtr p) : v(std::move(p)) {}
    Value(float f) : v(f) {}
    Value(GpuImagePtr p) : v(std::move(p)) {}
    Value(GpuChannelPtr p) : v(std::move(p)) {}

    bool empty() const;
    // Resolution carried by this value, or false for sizeless values (numbers, constant channels).
    bool size(int& w, int& h) const;
    bool onGpu() const { return v.index() >= 4; }
};

// The value with GPU images and channels downloaded (others unchanged).
Value toCpu(const Value& val);

// Implicit conversions between wire types. Return null / nullopt-like values on empty input.
//   Image   -> Channel : Rec.709 luminance
//   Channel -> Image   : grayscale, alpha 1
//   Number  -> Channel : constant broadcast
//   Number/constant -> Image : flat gray at (w, h)
ChannelPtr toChannel(const Value& val);
ImagePtr toImage(const Value& val, int w, int h);
float toNumber(const Value& val, float fallback);

// Regions of interest (see RoiWindow in graph/Node.h).
// The w x h block of src at (x0, y0), which must lie inside it.
ImagePtr cropImage(const Image& src, int x0, int y0, int w, int h);
// The same block of a sized value (image or channel); sizeless values are returned unchanged.
Value cropValue(const Value& v, int x0, int y0, int w, int h);
// A sized value stretched to w x h (nearest neighbour, like nodes sampling a mismatched input).
Value resampleValue(const Value& v, int w, int h);

bool canConvert(PinType from, PinType to);

inline float luminance(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
