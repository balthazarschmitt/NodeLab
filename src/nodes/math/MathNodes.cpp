#include "core/ColorMath.h"
#include "graph/Graph.h"
#include "gpu/PointOp.h"
#include "nodes/NodeUtil.h"
#include "nodes/math/LayerStack.h"

using namespace nodeutil;

namespace {

class MixNode : public Node {
public:
    REFRACTORY_NODE({"math.mix", "Mix", "Mix",
                  {{"A", PinType::Image}, {"B", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 0.5f, 0.0f, 1.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        resolveSize(in, ctx, w, h);
        ImagePtr a = toImage(in[0], w, h), b = toImage(in[1], w, h);
        if (!a && !b) return;
        if (!a) { out[0] = Value(b); return; }
        if (!b) { out[0] = Value(a); return; }
        ChannelPtr fac = channelOr(in[2], 0.5f);
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            ImageSampler sa{a.get(), w, h}, sb{b.get(), w, h};
            ChannelSampler sf = paramSampler(*this, 2, fac, w, h);
            for (int x = 0; x < w; ++x) {
                const float* pa = sa(x, y);
                const float* pb = sb(x, y);
                float f = sf(x, y);
                float* d = img->pixel(size_t(y) * w + x);
                for (int k = 0; k < 3; ++k) d[k] = clampColor(ctx.linear(), pa[k] + (pb[k] - pa[k]) * f);
                d[3] = clamp01(pa[3] + (pb[3] - pa[3]) * f);
            }
        });
        out[0] = Value(ImagePtr(img));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        if (in[0].empty() && in[1].empty()) return;
        gpu::PointOp op;
        resolveSize(in, ctx, op.w, op.h);
        op.body = R"(
    if (!has0) { out0 = img1(p); return; }
    if (!has1) { out0 = img0(p); return; }
    vec4 a = img0(p), b = img1(p);
    float f = par2(p);
    out0 = vec4(clampColor(uLinear, a.rgb + (b.rgb - a.rgb) * f), clamp01(a.a + (b.a - a.a) * f));)";
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

// Blend modes as in Blender's Mix Color node. B is blended over A.
enum BlendMode {
    Mix, Darken, Multiply, ColorBurn, Lighten, Screen, ColorDodge, Add, Overlay, SoftLight, LinearLight,
    Difference, Exclusion, Subtract, Divide, Hue, Saturation, ColorMode, ValueMode
};

float blendChannel(int mode, float a, float b) {
    switch (mode) {
        case Darken: return std::min(a, b);
        case Multiply: return a * b;
        case ColorBurn: return b <= 0.0f ? 0.0f : 1.0f - std::min(1.0f, (1.0f - a) / b);
        case Lighten: return std::max(a, b);
        case Screen: return 1.0f - (1.0f - a) * (1.0f - b);
        case ColorDodge: return b >= 1.0f ? 1.0f : std::min(1.0f, a / (1.0f - b));
        case Add: return a + b;
        case Overlay: return a < 0.5f ? 2.0f * a * b : 1.0f - 2.0f * (1.0f - a) * (1.0f - b);
        case SoftLight: return (1.0f - 2.0f * b) * a * a + 2.0f * b * a;
        case LinearLight: return a + 2.0f * b - 1.0f;
        case Difference: return std::fabs(a - b);
        case Exclusion: return a + b - 2.0f * a * b;
        case Subtract: return a - b;
        case Divide: return b <= 1e-6f ? a : a / b;
        default: return b;
    }
}

void blendPixel(int mode, const float* a, const float* b, float* out, bool lin) {
    using namespace colormath;
    if (mode >= Hue) {
        // HSV component swaps
        float ha, sa, va, hb, sb, vb;
        rgbToHsv(clampColor(lin, a[0]), clampColor(lin, a[1]), clampColor(lin, a[2]), ha, sa, va);
        rgbToHsv(clampColor(lin, b[0]), clampColor(lin, b[1]), clampColor(lin, b[2]), hb, sb, vb);
        switch (mode) {
            case Hue: hsvToRgb(hb, sa, va, out[0], out[1], out[2]); break;
            case Saturation: hsvToRgb(ha, sb, va, out[0], out[1], out[2]); break;
            case ColorMode: hsvToRgb(hb, sb, va, out[0], out[1], out[2]); break;
            default: hsvToRgb(ha, sa, vb, out[0], out[1], out[2]); break;
        }
        return;
    }
    for (int k = 0; k < 3; ++k) out[k] = blendChannel(mode, a[k], b[k]);
}

// Blend's modes in GLSL (blendChannel/blendPixel above). Callers pass the mode as a constant, so
// each shader keeps only its own modes' maths.
const char* const kBlendGlsl = R"(
float blendChannelM(int mode, float a, float b) {
    switch (mode) {
        case 1: return min(a, b);
        case 2: return a * b;
        case 3: return b <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - a) / b);
        case 4: return max(a, b);
        case 5: return 1.0 - (1.0 - a) * (1.0 - b);
        case 6: return b >= 1.0 ? 1.0 : min(1.0, a / (1.0 - b));
        case 7: return a + b;
        case 8: return a < 0.5 ? 2.0 * a * b : 1.0 - 2.0 * (1.0 - a) * (1.0 - b);
        case 9: return (1.0 - 2.0 * b) * a * a + 2.0 * b * a;
        case 10: return a + 2.0 * b - 1.0;
        case 11: return abs(a - b);
        case 12: return a + b - 2.0 * a * b;
        case 13: return a - b;
        case 14: return b <= 1e-6 ? a : a / b;
        default: return b;
    }
}
vec3 blendMode(int mode, vec3 a, vec3 b) {
    if (mode >= 15) {  // HSV component swaps
        vec3 ha = rgbToHsv(clampColor(uLinear, a)), hb = rgbToHsv(clampColor(uLinear, b));
        if (mode == 15) return hsvToRgb(vec3(hb.x, ha.y, ha.z));
        if (mode == 16) return hsvToRgb(vec3(ha.x, hb.y, ha.z));
        if (mode == 17) return hsvToRgb(vec3(hb.x, hb.y, ha.z));
        return hsvToRgb(vec3(ha.x, ha.y, hb.z));
    }
    return vec3(blendChannelM(mode, a.r, b.r), blendChannelM(mode, a.g, b.g), blendChannelM(mode, a.b, b.b));
}
)";

class BlendNode : public Node {
public:
    REFRACTORY_NODE({"math.blend", "Blend", "Mix",
                  {{"A", PinType::Image}, {"B", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f),
                   ParamDesc::Enum("Mode", Multiply,
                                   {"Mix", "Darken", "Multiply", "Color Burn", "Lighten", "Screen", "Color Dodge", "Add",
                                    "Overlay", "Soft Light", "Linear Light", "Difference", "Exclusion", "Subtract",
                                    "Divide", "Hue", "Saturation", "Color", "Value"}),
                   ParamDesc::Bool("Clamp", true)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        resolveSize(in, ctx, w, h);
        ImagePtr a = toImage(in[0], w, h), b = toImage(in[1], w, h);
        if (!a || !b) {
            if (a || b) out[0] = Value(a ? a : b);
            return;
        }
        const int mode = paramI(1);
        const bool clampOut = paramB(2);
        ChannelPtr fac = channelOr(in[2], 1.0f);
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            ImageSampler sa{a.get(), w, h}, sb{b.get(), w, h};
            ChannelSampler sf = paramSampler(*this, 2, fac, w, h);
            float blended[3];
            for (int x = 0; x < w; ++x) {
                const float* pa = sa(x, y);
                const float* pb = sb(x, y);
                float f = sf(x, y) * pb[3];  // B's alpha limits its influence
                blendPixel(mode, pa, pb, blended, ctx.linear());
                float* d = img->pixel(size_t(y) * w + x);
                for (int k = 0; k < 3; ++k) {
                    d[k] = pa[k] + (blended[k] - pa[k]) * f;
                    if (clampOut) d[k] = clamp01(d[k]);
                }
                d[3] = pa[3];
            }
        });
        out[0] = Value(ImagePtr(img));
    }

    bool gpuSupported(const EvalContext&, const std::vector<Value>&) const override { return true; }
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        if (in[0].empty() && in[1].empty()) return;
        gpu::PointOp op;
        resolveSize(in, ctx, op.w, op.h);
        // Mode and Clamp are compiled in, so each mode's shader holds only its own maths.
        op.functions = std::string(kBlendGlsl) + "const int MODE = " + std::to_string(paramI(1)) +
                       ";\nconst bool CLAMP_OUT = " + (paramB(2) ? "true" : "false") +
                       ";\nvec3 blendPixel(vec3 a, vec3 b) { return blendMode(MODE, a, b); }\n";
        op.body = R"(
    if (!has0) { out0 = img1(p); return; }
    if (!has1) { out0 = img0(p); return; }
    vec4 a = img0(p), b = img1(p);
    float f = par2(p) * b.a;  // B's alpha limits its influence
    vec3 d = a.rgb + (blendPixel(a.rgb, b.rgb) - a.rgb) * f;
    out0 = vec4(CLAMP_OUT ? clamp01(d) : d, a.a);)";
        gpu::runPoint(ctx, *this, op, in, out);
    }
};

}  // namespace

// ---------------------------------------------------------------- Layer Stack

namespace {

const std::vector<std::string>& layerModeNames() {
    // Blend's modes, with Mix called Normal as in Photoshop's layers.
    static const std::vector<std::string> names{
        "Normal", "Darken", "Multiply", "Color Burn", "Lighten", "Screen", "Color Dodge", "Add", "Overlay", "Soft Light",
        "Linear Light", "Difference", "Exclusion", "Subtract", "Divide", "Hue", "Saturation", "Color", "Value"};
    return names;
}

NodeInfo layerStackInfo(int n) {
    NodeInfo inf{LayerStackNode::kType, "Layer Stack", "Mix", {}, {{"Image", PinType::Image}}, {}};
    inf.compact = true;  // the opacities show on their pins; modes in the Inspector
    for (int i = 0; i < n; ++i) {
        const std::string s = std::to_string(i);
        inf.inputs.push_back({"Layer " + s, PinType::Image});
        inf.inputs.push_back({"Opacity " + s, PinType::Channel, i * LayerStackNode::kStride});
        inf.params.push_back(ParamDesc::Float("Opacity " + s, 1.0f, 0.0f, 1.0f));
        inf.params.push_back(ParamDesc::Enum("Mode " + s, Mix, layerModeNames()));
    }
    return inf;
}

}  // namespace

LayerStackNode::LayerStackNode() {
    info_ = staticInfo();
    initParams();
}

const NodeInfo& LayerStackNode::staticInfo() {
    static const NodeInfo inf = layerStackInfo(kMinLayers);
    return inf;
}

void LayerStackNode::setLayers(int n) {
    n = std::clamp(n, kMinLayers, kMaxLayers);
    info_ = layerStackInfo(n);
    const size_t old = params.size();
    params.resize(info_.params.size());
    for (size_t i = old; i < params.size(); ++i) params[i] = info_.params[i].def;
}

void LayerStackNode::resetParams() {
    for (size_t i = 0; i < params.size() && i < info_.params.size(); ++i) params[i] = info_.params[i].def;
}

void LayerStackNode::saveExtra(nlohmann::json& j) const {
    nlohmann::json ls = nlohmann::json::array();
    for (int i = 0; i < layers(); ++i)
        ls.push_back({{"opacity", paramF(i * kStride)}, {"mode", paramI(i * kStride + 1)}});
    j = {{"layers", ls}};
}

void LayerStackNode::loadExtra(const nlohmann::json& j) {
    if (!j.is_object()) return;
    const auto ls = j.find("layers");
    if (ls == j.end() || !ls->is_array()) return;
    setLayers(int(std::min<size_t>(ls->size(), kMaxLayers)));
    for (int i = 0; i < layers() && i < int(ls->size()); ++i) {
        const nlohmann::json& o = (*ls)[size_t(i)];
        if (!o.is_object()) continue;
        // Damaged values fall back to the defaults, as params do.
        if (const auto v = o.find("opacity"); v != o.end() && v->is_number() && std::isfinite(v->get<float>()))
            params[size_t(i * kStride)] = std::clamp(v->get<float>(), 0.0f, 1.0f);
        if (const auto v = o.find("mode"); v != o.end() && v->is_number_integer())
            params[size_t(i * kStride + 1)] = std::clamp(v->get<int>(), 0, int(layerModeNames().size()) - 1);
    }
}

bool LayerStackNode::layerUsed(const Graph& g, int layer) const {
    return g.inputLink(id, layer * kStride) || g.inputLink(id, layer * kStride + 1);
}

void LayerStackNode::linksChanged(Graph& g) {
    // Always an empty layer on top to connect the next image to.
    if (layerUsed(g, layers() - 1) && layers() < kMaxLayers) setLayers(layers() + 1);
}

void LayerStackNode::addLayer(Graph&) { setLayers(layers() + 1); }

void LayerStackNode::removeLayer(Graph& g, int layer) {
    if (layers() <= kMinLayers || layer < 0 || layer >= layers()) return;
    g.remapPins(id, false, [&](int p) { return p / kStride == layer ? -1 : p / kStride > layer ? p - kStride : p; });
    params.erase(params.begin() + layer * kStride, params.begin() + (layer + 1) * kStride);
    setLayers(layers() - 1);
}

void LayerStackNode::moveLayer(Graph& g, int layer, int dir) {
    const int other = layer + dir;
    if (layer < 0 || other < 0 || layer >= layers() || other >= layers()) return;
    g.remapPins(id, false, [&](int p) {
        const int l = p / kStride, k = p % kStride;
        return l == layer ? other * kStride + k : l == other ? layer * kStride + k : p;
    });
    for (int k = 0; k < kStride; ++k) std::swap(params[size_t(layer * kStride + k)], params[size_t(other * kStride + k)]);
    linksChanged(g);
}

// Each layer over the ones below: its blend with what's below (the layer itself where that is
// transparent), mixed in by the layer's alpha times its opacity, as Alpha Over does for Normal.
// The lowest connected layer's mode doesn't matter, and it sets the size.
void LayerStackNode::evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) {
    int w, h;
    resolveSize(in, ctx, w, h);
    struct Layer {
        ImagePtr img;
        ChannelPtr opacity;
        int pin, mode;
    };
    std::vector<Layer> ls;
    for (int i = 0; i < layers(); ++i) {
        ImagePtr img = toImage(in[size_t(i * kStride)], w, h);
        if (!img) continue;
        ls.push_back({img, channelOr(in[size_t(i * kStride + 1)], paramF(i * kStride)), i * kStride + 1,
                      paramI(i * kStride + 1)});
    }
    if (ls.empty()) return;
    const bool lin = ctx.linear();
    auto res = std::make_shared<Image>(w, h);
    parallelFor(h, [&](int y) {
        std::vector<ImageSampler> si;
        std::vector<ChannelSampler> so;
        for (const Layer& l : ls) {
            si.push_back(ImageSampler{l.img.get(), w, h});
            so.push_back(paramSampler(*this, l.pin, l.opacity, w, h));
        }
        float blended[3];
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (size_t i = 0; i < ls.size(); ++i) {
                const float* L = si[i](x, y);
                const float a = clamp01(L[3] * so[i](x, y));
                if (i == 0) {
                    acc[0] = L[0], acc[1] = L[1], acc[2] = L[2], acc[3] = a;
                    continue;
                }
                blendPixel(ls[i].mode, acc, L, blended, lin);
                const float below = clamp01(acc[3]);
                for (int k = 0; k < 3; ++k) {
                    const float b = L[k] + (blended[k] - L[k]) * below;
                    acc[k] += (b - acc[k]) * a;
                }
                acc[3] = a + acc[3] * (1.0f - a);
            }
            float* d = res->pixel(size_t(y) * w + x);
            for (int k = 0; k < 3; ++k) d[k] = clampColor(lin, acc[k]);
            d[3] = clamp01(acc[3]);
        }
    });
    out[0] = Value(ImagePtr(res));
}

bool LayerStackNode::gpuSupported(const EvalContext&, const std::vector<Value>& in) const {
    // Each sized input is a texture unit; GL 4.3 promises 16 to a compute shader, and fused
    // producers need some of them.
    int textures = 0, w, h;
    for (const Value& v : in) textures += v.size(w, h);
    return textures <= 8;
}

void LayerStackNode::evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) {
    gpu::PointOp op;
    resolveSize(in, ctx, op.w, op.h);
    // The connected layers and their modes are compiled in.
    std::string body = "    vec4 acc = vec4(0.0);\n";
    bool first = true;
    for (int i = 0; i < layers(); ++i) {
        if (in[size_t(i * kStride)].empty()) continue;
        const std::string pi = std::to_string(i * kStride), po = std::to_string(i * kStride + 1);
        body += "    {\n        vec4 L = img" + pi + "(p);\n        float a = clamp01(L.a * par" + po + "(p));\n";
        if (first) {
            body += "        acc = vec4(L.rgb, a);\n";
        } else {
            body += "        vec3 b = mix(L.rgb, blendMode(" + std::to_string(paramI(i * kStride + 1)) +
                    ", acc.rgb, L.rgb), clamp01(acc.a));\n"
                    "        acc.rgb += (b - acc.rgb) * a;\n"
                    "        acc.a = a + acc.a * (1.0 - a);\n";
        }
        body += "    }\n";
        first = false;
    }
    if (first) return;
    body += "    out0 = vec4(clampColor(uLinear, acc.rgb), clamp01(acc.a));";
    op.functions = kBlendGlsl;
    op.body = body;
    gpu::runPoint(ctx, *this, op, in, out);
}

void registerMathNodes(NodeRegistry& r) {
    r.add<MixNode>();
    r.add<BlendNode>();
    r.add<LayerStackNode>();
}
