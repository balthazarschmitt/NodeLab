#include "nodes/utility/UtilityNodes.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/ColorMath.h"
#include "core/Curve.h"
#include "graph/Evaluator.h"
#include "io/Exif.h"
#include "io/Export.h"
#include "io/ImageIO.h"
#include "io/Paths.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;
using namespace colormath;

namespace {

// Image of the given size (or the context size for sizeless inputs) with color from fn(value).
template <typename Fn>
ImagePtr colorFromChannel(const Node& node, EvalContext& ctx, const Value& v, float def, int pin, Fn&& fn) {
    ChannelPtr c = channelOr(v, def);
    int w = c->constant ? ctx.defaultW : c->w, h = c->constant ? ctx.defaultH : c->h;
    ChannelSampler s = paramSampler(node, pin, c, w, h);
    auto img = std::make_shared<Image>(w, h);
    parallelFor(h, [&](int y) {
        for (int x = 0; x < w; ++x) {
            float* d = img->pixel(size_t(y) * w + x);
            fn(s(x, y), d);
            d[3] = 1.0f;
        }
    });
    return img;
}

// ---------------------------------------------------------------- spectral converters

class WavelengthNode : public Node {
public:
    NODELAB_NODE({"conv.wavelength", "Wavelength", "Converter",
                  {{"Wavelength", PinType::Channel, 0}},
                  {{"Color", PinType::Image}},
                  {ParamDesc::Float("Wavelength", 550.0f, 360.0f, 830.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        // Lookup table in 1 nm steps; wavelengths are in nanometres.
        static const std::vector<std::array<float, 3>> lut = [] {
            std::vector<std::array<float, 3>> t(471);
            for (int i = 0; i < 471; ++i) wavelengthToRgb(360.0f + i, t[i][0], t[i][1], t[i][2]);
            return t;
        }();
        const bool lin = ctx.linear();
        out[0] = Value(colorFromChannel(*this, ctx, in[0], 550.0f, 0, [&](float nm, float* d) {
            float f = std::clamp(nm - 360.0f, 0.0f, 470.0f);
            int i = std::min(int(f), 469);
            float t = f - i;
            for (int k = 0; k < 3; ++k) d[k] = lut[i][k] + (lut[i + 1][k] - lut[i][k]) * t;
            if (lin)  // the tables hold display colours
                for (int k = 0; k < 3; ++k) d[k] = srgbToLinear(d[k]);
        }));
    }
};

class BlackbodyNode : public Node {
public:
    NODELAB_NODE({"conv.blackbody", "Blackbody", "Converter",
                  {{"Temperature", PinType::Channel, 0}},
                  {{"Color", PinType::Image}},
                  {ParamDesc::Float("Temperature", 3200.0f, 800.0f, 12000.0f)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        // Table over log(temperature) from 800 K to 40000 K.
        static const std::vector<std::array<float, 3>> lut = [] {
            std::vector<std::array<float, 3>> t(256);
            for (int i = 0; i < 256; ++i) {
                float k = 800.0f * std::pow(50.0f, i / 255.0f);
                blackbodyToRgb(k, t[i][0], t[i][1], t[i][2]);
            }
            return t;
        }();
        const bool lin = ctx.linear();
        out[0] = Value(colorFromChannel(*this, ctx, in[0], 3200.0f, 0, [&](float kelvin, float* d) {
            float f = std::clamp(std::log(std::max(kelvin, 800.0f) / 800.0f) / std::log(50.0f), 0.0f, 1.0f) * 255.0f;
            int i = std::min(int(f), 254);
            float t = f - i;
            for (int k = 0; k < 3; ++k) d[k] = lut[i][k] + (lut[i + 1][k] - lut[i][k]) * t;
            if (lin)  // the tables hold display colours
                for (int k = 0; k < 3; ++k) d[k] = srgbToLinear(d[k]);
        }));
    }
};

// ---------------------------------------------------------------- channel utilities

class NormalizeNode : public Node {
public:
    NODELAB_NODE({"conv.normalize", "Normalize", "Converter",
                  {{"Value", PinType::Channel}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::Float("Low %", 0.0f, 0.0f, 100.0f), ParamDesc::Float("High %", 100.0f, 0.0f, 100.0f)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ChannelPtr c = toChannel(in[0]);
        if (!c || c->constant) {
            if (c) out[0] = Value(c);
            return;
        }
        // Percentiles instead of plain min/max, so a few specular or black pixels don't set the
        // range (auto-exposure). 0 / 100 is Blender's min/max; the result is not clamped.
        std::vector<float> sorted(c->data);
        auto pct = [&](float p) {
            size_t k = size_t(std::clamp(p / 100.0f, 0.0f, 1.0f) * float(sorted.size() - 1) + 0.5f);
            std::nth_element(sorted.begin(), sorted.begin() + k, sorted.end());
            return sorted[k];
        };
        const float lo = pct(std::min(paramF(0), paramF(1))), hi = pct(std::max(paramF(0), paramF(1)));
        const float range = std::max(hi - lo, 1e-9f);
        out[0] = Value(ChannelPtr(makeChannel(c->w, c->h, [&](int x, int y) {
            return (c->data[size_t(y) * c->w + x] - lo) / range;
        })));
    }
};

class FloatCurveNode : public Node {
public:
    NODELAB_NODE({"conv.float_curve", "Float Curve", "Converter",
                  {{"Value", PinType::Channel, 0}, {"Factor", PinType::Channel, 1}},
                  {{"Value", PinType::Channel}},
                  {ParamDesc::Float("Value", 0.5f, 0.0f, 1.0f), ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f),
                   ParamDesc::CurveKeys("Curve", {"c:Curve"}, nlohmann::json{{"c", {{0.0, 0.0}, {1.0, 1.0}}}})}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        const nlohmann::json& cj = params[2];
        const std::vector<float> lut = curveLut(curveFromJson(cj.is_object() && cj.contains("c") ? cj["c"] : nlohmann::json()));
        ChannelPtr v = channelOr(in[0], 0.5f), f = channelOr(in[1], 1.0f);
        if (v->constant && f->constant) {
            float x = v->value;
            out[0] = Value(ChannelPtr(std::make_shared<Channel>(Channel::makeConstant(x + (lutLookup(lut, x) - x) * clamp01(f->value)))));
            return;
        }
        int w = !v->constant ? v->w : f->w, h = !v->constant ? v->h : f->h;
        ChannelSampler sv{v.get(), w, h}, sf = paramSampler(*this, 1, f, w, h);
        out[0] = Value(ChannelPtr(makeChannel(w, h, [&](int x, int y) {
            float a = sv(x, y);
            return a + (lutLookup(lut, a) - a) * sf(x, y);
        })));
    }
};

class SetAlphaNode : public Node {
public:
    NODELAB_NODE({"conv.set_alpha", "Set Alpha", "Converter",
                  {{"Image", PinType::Image}, {"Alpha", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Alpha", 1.0f, 0.0f, 1.0f), ParamDesc::Enum("Mode", 0, {"Replace Alpha", "Apply Mask (multiply color)"})}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const bool apply = paramI(1) == 1;
        ChannelPtr a = channelOr(in[1], 1.0f);
        ChannelSampler sa = paramSampler(*this, 1, a, src->w, src->h);
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            float al = sa(x, y);
            for (int k = 0; k < 3; ++k) d[k] = apply ? s[k] * al : s[k];
            d[3] = apply ? s[3] * al : al;
        })));
    }
};

// ---------------------------------------------------------------- compositing

class AlphaOverNode : public Node {
public:
    NODELAB_NODE({"math.alpha_over", "Alpha Over", "Mix",
                  {{"Background", PinType::Image}, {"Foreground", PinType::Image}, {"Factor", PinType::Channel, 0}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Factor", 1.0f, 0.0f, 1.0f), ParamDesc::Bool("Premultiplied", false)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        resolveSize(in, ctx, w, h);
        ImagePtr bg = toImage(in[0], w, h), fg = toImage(in[1], w, h);
        if (!bg || !fg) {
            if (bg || fg) out[0] = Value(bg ? bg : fg);
            return;
        }
        const bool premul = paramB(1);
        ChannelPtr fac = channelOr(in[2], 1.0f);
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            ImageSampler sb{bg.get(), w, h}, sf{fg.get(), w, h};
            ChannelSampler sa = paramSampler(*this, 2, fac, w, h);
            for (int x = 0; x < w; ++x) {
                const float* b = sb(x, y);
                const float* f = sf(x, y);
                float a = clamp01(f[3] * sa(x, y));
                float* d = img->pixel(size_t(y) * w + x);
                for (int k = 0; k < 3; ++k) d[k] = premul ? f[k] * sa(x, y) + b[k] * (1.0f - a) : f[k] * a + b[k] * (1.0f - a);
                d[3] = clamp01(a + b[3] * (1.0f - a));
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

// ---------------------------------------------------------------- utility

class RerouteNode : public Node {
public:
    NODELAB_NODE({"util.reroute", "Reroute", "Utility",
                  {{"", PinType::Image}},
                  {{"", PinType::Image}},
                  {}})
    // Passes the value through untouched (a channel stays a channel).
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override { out[0] = in[0]; }
};

class SwitchNode : public Node {
public:
    NODELAB_NODE({"util.switch", "Switch", "Utility",
                  {{"Off", PinType::Image}, {"On", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Bool("On", false)}})
    void evaluate(EvalContext&, const std::vector<Value>& in, std::vector<Value>& out) override { out[0] = in[paramB(0) ? 1 : 0]; }
};

class SplitNode : public Node {
public:
    NODELAB_NODE({"util.split", "Split (Compare)", "Utility",
                  {{"A", PinType::Image}, {"B", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Float("Position", 0.5f, 0.0f, 1.0f), ParamDesc::Enum("Orientation", 0, {"Vertical line", "Horizontal line"}),
                   ParamDesc::Bool("Show Line", true)}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        int w, h;
        resolveSize(in, ctx, w, h);
        ImagePtr a = toImage(in[0], w, h), b = toImage(in[1], w, h);
        if (!a || !b) {
            if (a || b) out[0] = Value(a ? a : b);
            return;
        }
        const bool vertical = paramI(1) == 0, line = paramB(2);
        const int cut = int(paramF(0) * (vertical ? w : h));
        auto img = std::make_shared<Image>(w, h);
        parallelFor(h, [&](int y) {
            ImageSampler sa{a.get(), w, h}, sb{b.get(), w, h};
            for (int x = 0; x < w; ++x) {
                int c = vertical ? x : y;
                const float* p = c < cut ? sa(x, y) : sb(x, y);
                float* d = img->pixel(size_t(y) * w + x);
                std::copy(p, p + 4, d);
                if (line && std::abs(c - cut) < 1) d[0] = d[1] = d[2] = 1.0f;
            }
        });
        out[0] = Value(ImagePtr(img));
    }
};

class ImageInfoNode : public Node {
public:
    NODELAB_NODE({"util.image_info", "Image Info", "Utility",
                  {{"Image", PinType::Image}},
                  {{"Width", PinType::Number}, {"Height", PinType::Number}, {"Aspect", PinType::Number}},
                  {}})
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr img = toImage(in[0], 0, 0);
        if (!img) return;
        // Report full-resolution size even while previewing the downscaled proxy.
        float w = std::round(img->w / ctx.scale), h = std::round(img->h / ctx.scale);
        out[0] = Value(w);
        out[1] = Value(h);
        out[2] = Value(w / std::max(h, 1.0f));
    }
};

class FileOutputNode : public Node {
public:
    NODELAB_NODE({"util.file_output", "File Output", "Utility",
                  {{"Image", PinType::Image}},
                  {},
                  {ParamDesc::SavePath("File"), ParamDesc::Enum("Format", 0, {"PNG", "JPEG", "TIFF", "OpenEXR"}),
                   ParamDesc::Bool("Enabled", true), ParamDesc::Enum("Color Depth", 0, {"8", "16"}),
                   ParamDesc::Enum("EXR Depth", 0, {"Float (Half)", "Float (Full)"}), ParamDesc::Int("Quality", 95, 1, 100)}})
    // Sink: files are written by File > Export (full resolution), not during previews.
    void evaluate(EvalContext&, const std::vector<Value>&, std::vector<Value>&) override {}
    // Like Blender's File Output, show only the chosen format's settings.
    bool paramHidden(int i) const override {
        const auto f = FileFormat(paramI(1));
        return (i == 3 && f != FileFormat::PNG && f != FileFormat::TIFF) || (i == 4 && f != FileFormat::EXR) ||
               (i == 5 && f != FileFormat::JPEG);
    }
    SaveOptions saveOptions(const Graph& g, int w, int h) const {
        SaveOptions o;
        o.format = FileFormat(std::clamp(paramI(1), 0, 3));
        o.depth = o.format == FileFormat::EXR ? (paramI(4) == 1 ? 32 : 16) : (paramI(3) == 1 ? 16 : 8);
        o.jpegQuality = paramI(5);
        if (o.format == FileFormat::JPEG) o.exif = exif::exportBlock(metadataSource(g), w, h);
        return o;
    }
};

}  // namespace

std::vector<std::string> writeFileOutputs(const Graph& g, ImageCache& cache) {
    EvalContext ctx;
    ctx.proxy = false;
    ctx.cache = &cache;
    initContextSize(g, ctx);
    Evaluator ev;  // shared, so upstream work common to several outputs runs once
    return writeFileOutputs(g, ev, ctx);
}

std::vector<std::string> writeFileOutputs(const Graph& g, Evaluator& ev, EvalContext& ctx) {
    std::vector<std::string> report;
    for (const auto& [id, n] : g.nodes()) {
        if (n->info().type != FileOutputNode::staticInfo().type || !n->paramB(2)) continue;
        std::string path = n->paramS(0);
        if (path.empty()) {
            report.push_back("File Output: no file chosen");
            continue;
        }
        // Make the extension match the chosen format.
        auto p = u8ToPath(path);
        p.replace_extension(formatExtension(FileFormat(std::clamp(n->paramI(1), 0, 3))));
        path = pathToU8(p);
        try {
            ImagePtr img = ev.evaluateDisplay(g, id, ctx);
            std::string err;
            if (!img) report.push_back("File Output: nothing connected (" + path + ")");
            else if (!saveRendered(path, img, ctx.colorManagement,
                                   static_cast<const FileOutputNode&>(*n).saveOptions(g, img->w, img->h), err))
                report.push_back("File Output: " + err + " (" + path + ")");
            else report.push_back("Wrote " + path);
        } catch (const EvalCancelled&) {
            throw;
        } catch (const std::exception& e) {
            report.push_back(std::string("File Output: ") + e.what());
        }
    }
    return report;
}

void registerUtilityNodes(NodeRegistry& r) {
    r.add<WavelengthNode>();
    r.add<BlackbodyNode>();
    r.add<NormalizeNode>();
    r.add<FloatCurveNode>();
    r.add<SetAlphaNode>();
    r.add<AlphaOverNode>();
    r.add<RerouteNode>();
    r.add<SwitchNode>();
    r.add<SplitNode>();
    r.add<ImageInfoNode>();
    r.add<FileOutputNode>();
}
