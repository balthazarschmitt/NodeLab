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
    //   lutLookup(offset, n, x)  the n-entry table at lut[offset] at x in 0..1, as Curve.h's
    //   lutAt(i)    lut[i]: exact per-row or per-column values computed on the CPU
    //   uLinear     scene-linear project (EvalContext::linear)
    //   uOrigin, uFull  the buffer's origin in the full image and its size (nodeutil::frameOf)
    // and assigns out<k>: vec4 for Image outputs, float for Channel outputs.
    std::string body;
    std::string functions;  // GLSL placed before eval (helpers the body calls)
    std::vector<float> params;
    std::vector<float> lut;  // curve tables (read through lutLookup), or values read through lutAt
    // Values for unconnected pins, as the CPU's channelOr(in[i], default): such a pin reads as that
    // constant (and has<i> is true). Pins beyond the vector, or NaN entries, read as missing.
    std::vector<float> defaults;
    // Pins read at other pixels than p (filters, transforms). For these the body can also call:
    //   fetch<i>(q)     the pin as RGBA at pixel q of its own buffer, clamped to the edges (as img<i>)
    //   fetchCh<i>(q)   the same as one value (as ch<i>)
    //   bilinear<i>(xy, transparent)  imageops::sampleBilinear at continuous pixel coordinates
    //   size<i>         the pin's buffer size (the output size for constants and missing pins)
    // They read textures: a pending producer runs on its own first instead of being fused.
    std::vector<int> gather;
    // False keeps the outputs out of later shaders. For costly bodies (many taps per pixel), which a
    // consumer reading its input more than once would otherwise run again.
    bool inlinable = true;
    // Image outputs at full float precision even in half-precision runs (sums, intermediates).
    bool full = false;
    int w = 0, h = 0;  // output size
};

// Runs op for `node` (its outputs give the out<k> types) on the device; out receives GPU values.
// Needs a gpu::Scope. Throws gpu::Error on device failures.
//
// The values are pending (GpuValue::texture runs them): a point op whose input is still pending,
// and of its size, compiles the producer's code into its own shader (fusion, like Blender's
// GPU compositor's pixel operations). A chain of per-pixel nodes then runs as one shader,
// reading its inputs once and writing one texture, at full float precision in between. Nodes are
// immutable once pending, so fusing never changes a result, only where it is computed.
void runPoint(EvalContext& ctx, const Node& node, const PointOp& op, const std::vector<Value>& in,
              std::vector<Value>& out);

// runPoint over the pixels of input `pin` (what mapImage-style nodes do with toImage(in[0], 0, 0)).
// Throws when that input has no pixels: gpuSupported should check sizedValue(in[pin]).
void runOver(EvalContext& ctx, const Node& node, PointOp op, const std::vector<Value>& in, std::vector<Value>& out,
             int pin = 0);

// One pass of a node that runs several (blur stages, sums, streaks): runPoint for outputs that
// aren't the node's own. out<k> is an Image where outImage[k] is true, else a Channel; par<i> is
// unclamped. The results are pending like runPoint's.
std::vector<Value> runPass(EvalContext& ctx, const PointOp& op, const std::vector<Value>& in,
                           const std::vector<bool>& outImage);

// GLSL shared by GPU kernels, as core/ColorMath.h: luminance, clamp01, clampColor, finiteOr0,
// powPos, sRGB transfer, HSV, HSL, Lab, YCbCr, YUV; as core/ColorScience.h: rgbToOklab, oklabToRgb,
// compressToGamut. Also atan2C (C's atan2).
extern const char* const kGlslCommon;

// Whether v is a point op's result that hasn't run yet.
bool pending(const Value& v);
// Stages that have run inside another stage's shader so far (statistics and tests).
int fusedStages();
// Runs v if pending, so its pixels exist as a texture (the evaluator does this for values with
// several readers, which would otherwise each compute them again).
void materialize(const Value& v);

// True when a value can feed a GPU kernel: empty, a number, a channel or an image.
bool gpuInput(const Value& v);
// True when v carries pixels (an image or a sized channel, on either side).
bool sizedValue(const Value& v);

}  // namespace gpu
