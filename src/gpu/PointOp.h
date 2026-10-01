#pragma once
// Per-pixel nodes on the GPU. A node describes its maths as a GLSL body; runPoint wraps it in a
// compute shader with accessors for its inputs that behave like the CPU helpers in NodeUtil.h, so
// the GPU version of a node is a few lines of GLSL next to its C++ loop.
#include <string>
#include <vector>

#include "graph/Node.h"

namespace gpu {

struct PointOp {
    // The body of `void eval(ivec2 p, out ... out0, ...)`, run for each output pixel p (in the
    // node's buffer). It reads, for each input pin i:
    //   img<i>(p)   the pin as RGBA: channels grey with alpha 1, numbers flat (toImage)
    //   ch<i>(p)    the pin as one value: images give Rec.709 luminance (toChannel)
    //   par<i>(p)   ch<i>(p) clamped to the range of the param backing the pin (paramSampler)
    //   has<i>      whether the pin has a value
    // Inputs of another size are sampled nearest, as ImageSampler/ChannelSampler do. Also:
    //   P[k]        params[k] below
    //   uLinear     scene-linear project (EvalContext::linear)
    //   uOrigin, uFull  the buffer's origin in the full image and its size (nodeutil::frameOf)
    // and assigns out<k>: vec4 for Image outputs, float for Channel outputs.
    std::string body;
    std::string functions;  // GLSL placed before eval (helpers the body calls)
    std::vector<float> params;
    int w = 0, h = 0;  // output size
};

// Runs op for `node` (its outputs give the out<k> types) on the device; out receives GPU values.
// Needs a gpu::Scope. Throws gpu::Error on device failures.
void runPoint(EvalContext& ctx, const Node& node, const PointOp& op, const std::vector<Value>& in,
              std::vector<Value>& out);

// GLSL shared by GPU kernels: luminance, clamp01, clampColor, finiteOr0, HSV (as core/ColorMath.h).
extern const char* const kGlslCommon;

// True when a value can feed a GPU kernel: empty, a number, a channel or an image.
bool gpuInput(const Value& v);
// True when v carries pixels (an image or a sized channel, on either side).
bool sizedValue(const Value& v);

}  // namespace gpu
