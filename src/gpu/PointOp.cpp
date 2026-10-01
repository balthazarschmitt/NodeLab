#include "gpu/PointOp.h"

#include <cstdio>

#include "gpu/Device.h"
#include "gpu/GL.h"
#include "nodes/NodeUtil.h"

namespace gpu {

const char* const kGlslCommon = R"GLSL(
float luminance(vec3 c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
float clamp01(float v) { return clamp(v, 0.0, 1.0); }
vec3 clamp01(vec3 v) { return clamp(v, 0.0, 1.0); }
// Colour nodes: 0..1 in legacy projects, only negatives removed in scene-linear ones.
float clampColor(bool lin, float v) { return lin ? max(v, 0.0) : clamp(v, 0.0, 1.0); }
vec3 clampColor(bool lin, vec3 v) { return lin ? max(v, 0.0) : clamp(v, 0.0, 1.0); }
// NaN and infinities become 0, by their bits (compilers may assume floats are finite).
float finiteOr0(float v) { return (floatBitsToUint(v) & 0x7F800000u) == 0x7F800000u ? 0.0 : v; }
const float kNaN = uintBitsToFloat(0x7FC00000u);
vec3 rgbToHsv(vec3 c) {
    float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
    float d = mx - mn;
    float s = mx > 1e-6 ? d / mx : 0.0;
    if (d < 1e-6) return vec3(0.0, s, mx);
    float h;
    if (mx == c.r) h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
    else if (mx == c.g) h = (c.b - c.r) / d + 2.0;
    else h = (c.r - c.g) / d + 4.0;
    return vec3(h / 6.0, s, mx);
}
vec3 hsvToRgb(vec3 hsv) {
    float h = hsv.x - floor(hsv.x), s = hsv.y, v = hsv.z;
    float f = h * 6.0;
    int i = int(f) % 6;
    f -= floor(f);
    float p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
    if (i == 0) return vec3(v, t, p);
    if (i == 1) return vec3(q, v, p);
    if (i == 2) return vec3(p, v, t);
    if (i == 3) return vec3(p, q, v);
    if (i == 4) return vec3(t, p, v);
    return vec3(v, p, q);
}
)GLSL";

bool gpuInput(const Value&) {
    return true;  // every kind of value has a GPU form (sized ones are uploaded)
}

bool sizedValue(const Value& v) {
    int w, h;
    return v.size(w, h);
}

namespace {

enum class Kind { None, Constant, ImageTex, ChannelTex };

std::string num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.9g", double(v));
    std::string s = b;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";  // GLSL float literal
    return s;
}

}  // namespace

void runPoint(EvalContext& ctx, const Node& node, const PointOp& op, const std::vector<Value>& in,
              std::vector<Value>& out) {
    const NodeInfo& info = node.info();
    const int w = op.w, h = op.h;
    if (w <= 0 || h <= 0) throw Error("GPU: empty output");

    // What each input is, and its texture or constant.
    std::vector<Kind> kinds(in.size(), Kind::None);
    std::vector<const Texture*> texes(in.size(), nullptr);
    std::vector<int> tw(in.size()), th(in.size());
    std::vector<float> consts(in.size(), 0.0f);
    std::vector<Value> keep;  // uploads made here (the evaluator normally does them)
    for (size_t i = 0; i < in.size(); ++i) {
        Value v = in[i];
        if (auto p = std::get_if<ImagePtr>(&v.v); p && *p) keep.push_back(v = Value(upload(**p, ctx.gpuHalf ? Format::RGBA16F : Format::RGBA32F)));
        if (auto p = std::get_if<ChannelPtr>(&v.v); p && *p && !(*p)->constant) keep.push_back(v = Value(upload(**p)));
        if (auto p = std::get_if<GpuImagePtr>(&v.v); p && *p) {
            kinds[i] = Kind::ImageTex;
            texes[i] = (*p)->tex.get();
            tw[i] = (*p)->w, th[i] = (*p)->h;
        } else if (auto p = std::get_if<GpuChannelPtr>(&v.v); p && *p) {
            kinds[i] = Kind::ChannelTex;
            texes[i] = (*p)->tex.get();
            tw[i] = (*p)->w, th[i] = (*p)->h;
        } else if (auto p = std::get_if<float>(&v.v)) {
            kinds[i] = Kind::Constant;
            consts[i] = *p;
        } else if (auto p = std::get_if<ChannelPtr>(&v.v); p && *p) {
            kinds[i] = Kind::Constant;
            consts[i] = (*p)->value;
        }
    }

    // The shader: header, input accessors, outputs, the node's code, main.
    std::string s = "#version 430\nlayout(local_size_x = 16, local_size_y = 16) in;\n";
    s += "uniform ivec2 uSize;\nuniform ivec2 uOrigin;\nuniform ivec2 uFull;\nuniform bool uLinear;\n";
    s += "uniform float P[" + std::to_string(std::max<size_t>(1, op.params.size())) + "];\n";
    s += kGlslCommon;
    for (size_t i = 0; i < in.size(); ++i) {
        const std::string n = std::to_string(i);
        std::string img, ch;
        switch (kinds[i]) {
            case Kind::None:
                img = "vec4(0.0)";
                ch = "0.0";
                break;
            case Kind::Constant:
                s += "uniform float uC" + n + ";\n";
                img = "vec4(vec3(uC" + n + "), 1.0)";
                ch = "uC" + n;
                break;
            case Kind::ImageTex:
            case Kind::ChannelTex: {
                s += "layout(binding = " + n + ") uniform sampler2D uTex" + n + ";\nuniform ivec2 uSz" + n + ";\n";
                // Nearest sample of a mismatched size, with the CPU's integer maths.
                s += "ivec2 at" + n + "(ivec2 p) { return uSz" + n + " == uSize ? p : min(uSz" + n + " - 1, p * uSz" + n +
                     " / max(uSize, ivec2(1))); }\n";
                const std::string fetch = "texelFetch(uTex" + n + ", at" + n + "(p), 0)";
                if (kinds[i] == Kind::ImageTex) {
                    img = fetch;
                    ch = "luminance(" + fetch + ".rgb)";
                } else {
                    img = "vec4(vec3(" + fetch + ".r), 1.0)";
                    ch = fetch + ".r";
                }
                break;
            }
        }
        s += "const bool has" + n + " = " + (kinds[i] == Kind::None ? "false" : "true") + ";\n";
        s += "vec4 img" + n + "(ivec2 p) { return " + img + "; }\n";
        s += "float ch" + n + "(ivec2 p) { return " + ch + "; }\n";
        // paramSampler's range: the param backing the pin, if any.
        float lo = -1e30f, hi = 1e30f;
        if (i < info.inputs.size() && info.inputs[i].fallbackParam >= 0) {
            const ParamDesc& d = info.params[size_t(info.inputs[i].fallbackParam)];
            lo = d.hardMin;
            hi = d.hardMax;
        }
        s += "float par" + n + "(ivec2 p) { return clamp(ch" + n + "(p), " + num(lo) + ", " + num(hi) + "); }\n";
    }
    std::vector<Format> formats;
    std::string evalParams, mainDecl, mainCall, mainStore;
    for (size_t k = 0; k < info.outputs.size(); ++k) {
        const std::string n = std::to_string(k);
        const bool image = info.outputs[k].type == PinType::Image;
        if (!image && info.outputs[k].type != PinType::Channel) throw Error("GPU: point ops output images and channels");
        const Format f = image ? (ctx.gpuHalf ? Format::RGBA16F : Format::RGBA32F) : Format::R32F;
        formats.push_back(f);
        s += "layout(" + std::string(glslFormat(f)) + ", binding = " + n + ") uniform writeonly image2D uOut" + n + ";\n";
        const char* type = image ? "vec4" : "float";
        evalParams += std::string(k ? ", " : "") + "out " + type + " out" + n;
        mainDecl += std::string("    ") + type + " o" + n + (image ? " = vec4(0.0, 0.0, 0.0, 1.0)" : " = 0.0") + ";\n";
        mainCall += std::string(k ? ", " : "") + "o" + n;
        mainStore += "    imageStore(uOut" + n + ", p, " + (image ? "o" + n : "vec4(o" + n + ")") + ");\n";
    }
    s += op.functions;
    s += "\nvoid eval(ivec2 p, " + evalParams + ") {\n" + op.body + "\n}\n";
    s += "void main() {\n    ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n    if (p.x >= uSize.x || p.y >= uSize.y) return;\n";
    s += mainDecl + "    eval(p, " + mainCall + ");\n" + mainStore + "}\n";

    const unsigned prog = program(s);
    gl::UseProgram(prog);
    auto loc = [&](const std::string& name) { return gl::GetUniformLocation(prog, name.c_str()); };
    const nodeutil::PixelFrame fr = nodeutil::frameOf(ctx, w, h);
    gl::Uniform2i(loc("uSize"), w, h);
    gl::Uniform2i(loc("uOrigin"), fr.x0, fr.y0);
    gl::Uniform2i(loc("uFull"), fr.fullW, fr.fullH);
    gl::Uniform1i(loc("uLinear"), ctx.linear() ? 1 : 0);
    if (!op.params.empty()) gl::Uniform1fv(loc("P"), int(op.params.size()), op.params.data());
    // Outputs first: a new texture is bound while it is created, which would replace an input's.
    std::vector<TexturePtr> outTex;
    for (Format f : formats) outTex.push_back(allocate(w, h, f));
    for (size_t i = 0; i < in.size(); ++i) {
        const std::string n = std::to_string(i);
        if (kinds[i] == Kind::Constant) gl::Uniform1f(loc("uC" + n), consts[i]);
        if (texes[i]) {
            gl::ActiveTexture(gl::TEXTURE0 + unsigned(i));
            gl::BindTexture(gl::TEXTURE_2D, texes[i]->id());
            gl::Uniform2i(loc("uSz" + n), tw[i], th[i]);
        }
    }
    gl::ActiveTexture(gl::TEXTURE0);
    out.assign(info.outputs.size(), Value());
    for (size_t k = 0; k < formats.size(); ++k) {
        const TexturePtr& t = outTex[k];
        gl::BindImageTexture(unsigned(k), t->id(), 0, 0, 0, gl::WRITE_ONLY, gl::GLenum(formats[k] == Format::RGBA16F ? gl::RGBA16F : formats[k] == Format::RGBA32F ? gl::RGBA32F : gl::R32F));
        if (formats[k] == Format::R32F) {
            auto c = std::make_shared<GpuChannel>();
            c->tex = t, c->w = w, c->h = h;
            out[k] = Value(GpuChannelPtr(c));
        } else {
            auto im = std::make_shared<GpuImage>();
            im->tex = t, im->w = w, im->h = h;
            out[k] = Value(GpuImagePtr(im));
        }
    }
    dispatch(w, h);
    if (gl::GetError() != gl::NO_ERROR) throw Error("GPU: point op failed");
}

}  // namespace gpu
