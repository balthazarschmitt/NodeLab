#include "gpu/Display.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "gpu/Device.h"
#include "gpu/GL.h"
#include "gpu/PointOp.h"

namespace gpu {

namespace {

// Mirrors colormgmt::viewTransform (ColorManagement.cpp) and displayBytes/Histogram::compute
// (ui/ImageView.cpp). Matrices are written by rows: v * mat3(rows) is the row-major product.
const char* const kDisplaySource = R"(
layout(local_size_x = 16, local_size_y = 16) in;
layout(binding = 0) uniform sampler2D uSrc;
layout(rgba8, binding = 0) writeonly uniform image2D uOut;
layout(std430, binding = 1) buffer Hist { uint hist[]; };
uniform ivec2 uSize;
uniform int uLinear, uView, uLook, uChannel, uClipping, uHistogram, uStep;
uniform float uExposure, uGamma;

const mat3 k709To2020 = mat3(vec3(0.6274039, 0.3292830, 0.0433131), vec3(0.0690973, 0.9195404, 0.0113623),
                             vec3(0.0163914, 0.0880133, 0.8955953));
const mat3 k2020To709 = mat3(vec3(1.6604910, -0.5876411, -0.0728499), vec3(-0.1245505, 1.1328999, -0.0083494),
                             vec3(-0.0181508, -0.1005789, 1.1187297));
const mat3 kAgxInset = mat3(vec3(0.856627153315983, 0.0951212405381588, 0.0482516061458583),
                            vec3(0.137318972929847, 0.761241990602591, 0.101439036467562),
                            vec3(0.11189821299995, 0.0767994186031903, 0.811302368396859));
const mat3 kAgxOutset = mat3(vec3(1.1271005818144368, -0.11060664309660323, -0.016493938717834573),
                             vec3(-0.1413297634984383, 1.157823702216272, -0.016493938717834257),
                             vec3(-0.14132976349843826, -0.11060664309660294, 1.2519364065950405));
const float kAgxMinEv = -12.47393, kAgxMaxEv = 4.026069;

float agxContrast(float x) {
    float x2 = x * x, x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

vec3 agx(vec3 s) {
    vec3 v = (s * k709To2020) * kAgxInset;
    for (int k = 0; k < 3; ++k)
        v[k] = agxContrast(clamp((log2(max(v[k], 1e-10)) - kAgxMinEv) / (kAgxMaxEv - kAgxMinEv), 0.0, 1.0));
    if (uLook != 0) {
        float luma = 0.2126 * v.r + 0.7152 * v.g + 0.0722 * v.b;
        float power = uLook == 1 ? 1.35 : 1.0, sat = uLook == 1 ? 1.4 : 0.0;
        v = luma + sat * (powPos(max(v, 0.0), vec3(power)) - luma);
    }
    vec3 c = powPos(max(v * kAgxOutset, 0.0), vec3(2.2));
    return c * k2020To709;
}

vec3 viewTransform(vec3 in_) {
    vec3 s = in_ * uExposure;
    vec3 o;
    if (uView == 1) o = linearToSrgb(clamp01(agx(s)));
    else if (uView == 2) o = clamp01(s);
    else o = linearToSrgb(clamp01(s));
    if (uGamma != 1.0) o = powPos(o, vec3(1.0 / uGamma));
    return o;
}

// std::lround(clamp(v, 0, 1) * 255): halves round up, exactly (f - int(f) is exact below 256).
uint toByte(float v) {
    float f = clamp(v, 0.0, 1.0) * 255.0;
    uint t = uint(f);
    return f - float(t) >= 0.5 ? t + 1u : t;
}

shared uint counts[1026];

void main() {
    uint li = gl_LocalInvocationIndex;
    if (uHistogram != 0) {
        for (uint i = li; i < 1026u; i += 256u) counts[i] = 0u;
        barrier();
    }
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (all(lessThan(p, uSize))) {
        vec4 t = texelFetch(uSrc, p, 0);
        vec3 c = uChannel != 0 ? vec3(t.r) : t.rgb;
        if (uLinear != 0) c = viewTransform(c);
        uvec3 b = uvec3(toByte(c.r), toByte(c.g), toByte(c.b));
        if (uHistogram != 0 && p.y % uStep == 0) {
            atomicAdd(counts[b.r], 1u);
            atomicAdd(counts[256u + b.g], 1u);
            atomicAdd(counts[512u + b.b], 1u);
            atomicAdd(counts[768u + toByte(0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b)], 1u);
            if (b.r == 255u || b.g == 255u || b.b == 255u) counts[1024] = 1u;
            if (b.r == 0u && b.g == 0u && b.b == 0u) counts[1025] = 1u;
        }
        uvec3 o = b;
        if (uClipping != 0) {
            uint mx = max(b.r, max(b.g, b.b));
            if (mx == 255u) o = uvec3(255u, 0u, 0u);
            else if (mx == 0u) o = uvec3(0u, 90u, 255u);
        }
        // Exact bytes: unorm stores round v * 255 to the nearest integer.
        // Alpha as displayBytes stores it (channels are opaque), for the viewer's checkerboard.
        float a = uChannel != 0 ? 1.0 : (t.a >= 0.0 ? min(t.a, 1.0) : 0.0);
        imageStore(uOut, p, vec4(vec3(o) / 255.0, a));
    }
    if (uHistogram != 0) {
        barrier();
        for (uint i = li; i < 1024u; i += 256u)
            if (counts[i] != 0u) atomicAdd(hist[i], counts[i]);
        if (li == 0u) {
            if (counts[1024] != 0u) hist[1024] = 1u;
            if (counts[1025] != 0u) hist[1025] = 1u;
        }
    }
}
)";

struct Buffer {
    unsigned id = 0;
    Buffer() { gl::GenBuffers(1, &id); }
    ~Buffer() { gl::DeleteBuffers(1, &id); }
};

}  // namespace

DisplayResult display(const Value& scene, const ColorManagement& cm, bool clipping, bool histogram, bool keepTexture) {
    TexturePtr tex;
    bool channel = false;
    if (auto i = std::get_if<GpuImagePtr>(&scene.v); i && *i) tex = (*i)->texture();
    else if (auto c = std::get_if<GpuChannelPtr>(&scene.v); c && *c) tex = (*c)->texture(), channel = true;
    else if (auto ci = std::get_if<ImagePtr>(&scene.v); ci && *ci && !(*ci)->empty())
        tex = upload(**ci, Format::RGBA32F)->texture();
    else if (auto cc = std::get_if<ChannelPtr>(&scene.v); cc && *cc && !(*cc)->constant)
        tex = upload(**cc)->texture(), channel = true;
    if (!tex) throw Error("GPU: nothing to display");

    DisplayResult out;
    out.w = tex->w(), out.h = tex->h();
    const size_t n = size_t(out.w) * out.h;
    static const std::string source = std::string("#version 430\n") + kGlslCommon + kDisplaySource;
    const unsigned prog = program(source);
    // Bytes go to a texture: read back through a pack buffer, which is fast, where a buffer the
    // shader wrote is read through uncached memory (53 ms instead of a few for 2.7 MP).
    const TexturePtr dst = allocate(out.w, out.h, Format::RGBA8);
    Buffer hist;
    if (histogram) {
        out.histogram.assign(1026, 0);
        gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, hist.id);
        gl::BufferData(gl::SHADER_STORAGE_BUFFER, gl::GLsizeiptr(out.histogram.size() * 4), out.histogram.data(),
                       gl::DYNAMIC_READ);
        gl::BindBufferBase(gl::SHADER_STORAGE_BUFFER, 1, hist.id);
    }
    gl::UseProgram(prog);
    auto i1 = [&](const char* name, int v) { gl::Uniform1i(gl::GetUniformLocation(prog, name), v); };
    auto f1 = [&](const char* name, float v) { gl::Uniform1f(gl::GetUniformLocation(prog, name), v); };
    gl::Uniform2i(gl::GetUniformLocation(prog, "uSize"), out.w, out.h);
    i1("uLinear", cm.linear), i1("uView", cm.view), i1("uLook", cm.look), i1("uChannel", channel);
    i1("uClipping", clipping), i1("uHistogram", histogram);
    i1("uStep", std::max(1, int(n / 2000000)));
    f1("uExposure", std::exp2(cm.exposure)), f1("uGamma", cm.gamma);
    bindTexture(0, *tex);
    bindImage(0, *dst);
    dispatch(out.w, out.h);

    if (keepTexture) {
        gl::BindTexture(gl::TEXTURE_2D, dst->id());
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
        gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::NEAREST);
        gl::BindTexture(gl::TEXTURE_2D, 0);
        out.texture = dst;
    } else {
        out.bytes = downloadBytes(*dst);
    }
    if (histogram) {
        gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, hist.id);
        gl::GetBufferSubData(gl::SHADER_STORAGE_BUFFER, 0, gl::GLsizeiptr(out.histogram.size() * 4),
                             out.histogram.data());
    }
    gl::BindBuffer(gl::SHADER_STORAGE_BUFFER, 0);
    if (gl::GetError() != gl::NO_ERROR) throw Error("GPU: display conversion failed");
    // The UI's context draws it next: it must see the finished pixels (a shared context only
    // sees another's writes once they have completed).
    if (keepTexture) gl::Finish();
    return out;
}

}  // namespace gpu
