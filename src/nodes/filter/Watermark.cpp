// Lightroom's export watermark: a line of text (or a logo image wired to Logo) placed at one of
// nine anchors with an inset, an opacity and a drop shadow. Sizes are fractions of the image's
// short edge, so the preview matches the export. Text is drawn with stb_truetype from a Windows
// system font.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "graph/NodeRegistry.h"
#include "nodes/ImageOps.h"
#include "nodes/NodeUtil.h"

using namespace nodeutil;

namespace {

}  // namespace

// Built in (cmake/EmbedText.cmake), for systems without the Windows fonts.
extern const char* const kFontRoboto;
extern const std::size_t kFontRobotoSize;
extern const char* const kFontCousine;
extern const std::size_t kFontCousineSize;

namespace {

// The font file's bytes, read once (stb_truetype reads glyphs straight from them): the system's
// font, else the built-in Roboto (Cousine for Monospace).
const std::vector<unsigned char>* fontData(int which) {
    static const char* const kFiles[][2] = {{"segoeui.ttf", "arial.ttf"},
                                            {"segoeuib.ttf", "arialbd.ttf"},
                                            {"georgia.ttf", "times.ttf"},
                                            {"consola.ttf", "cour.ttf"}};
    static std::mutex m;
    static std::map<int, std::vector<unsigned char>> cache;
    std::lock_guard lock(m);
    if (auto it = cache.find(which); it != cache.end()) return it->second.empty() ? nullptr : &it->second;
    const char* windir = std::getenv("WINDIR");
    const std::string dir = std::string(windir ? windir : "C:\\Windows") + "\\Fonts\\";
    std::vector<unsigned char>& data = cache[which];
    for (const char* file : kFiles[which]) {
        std::ifstream f(dir + file, std::ios::binary);
        if (!f) continue;
        data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        if (!data.empty()) break;
    }
    if (data.empty()) {
        const bool mono = which == 3;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(mono ? kFontCousine : kFontRoboto);
        data.assign(p, p + (mono ? kFontCousineSize : kFontRobotoSize));
    }
    return data.empty() ? nullptr : &data;
}

std::vector<int> codepoints(const std::string& s) {
    std::vector<int> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (n < 0 || i + size_t(n) >= s.size() + (n == 0 ? 1 : 0)) {
            ++i;  // a stray byte: skip it
            continue;
        }
        int cp = n == 0 ? c : c & (0x3f >> n);
        bool ok = true;
        for (int k = 1; k <= n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + size_t(k)]);
            if ((d >> 6) != 2) ok = false;
            cp = (cp << 6) | (d & 0x3f);
        }
        i += size_t(n) + 1;
        if (ok) out.push_back(cp);
    }
    return out;
}

// A coverage mask (0..1), w x h.
struct Mask {
    int w = 0, h = 0;
    std::vector<float> v;
};

Mask renderText(const std::string& text, int font, float pixelHeight) {
    Mask m;
    const std::vector<unsigned char>* data = fontData(font);
    const std::vector<int> cps = codepoints(text);
    if (!data || cps.empty() || pixelHeight < 1.0f) return m;
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data->data(), stbtt_GetFontOffsetForIndex(data->data(), 0))) return m;
    const float scale = stbtt_ScaleForPixelHeight(&info, pixelHeight);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    // Pen positions first, for the block's width.
    std::vector<float> xs;
    float pen = 0.0f;
    for (size_t i = 0; i < cps.size(); ++i) {
        xs.push_back(pen);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&info, cps[i], &adv, &lsb);
        pen += adv * scale;
        if (i + 1 < cps.size()) pen += scale * stbtt_GetCodepointKernAdvance(&info, cps[i], cps[i + 1]);
    }
    const int base = int(std::ceil(ascent * scale));
    m.w = std::max(1, int(std::ceil(pen)) + 2);
    m.h = std::max(1, base + int(std::ceil(-descent * scale)) + 2);
    if (double(m.w) * m.h > 64e6) return Mask{};  // absurd sizes: draw nothing rather than run out of memory
    m.v.assign(size_t(m.w) * size_t(m.h), 0.0f);
    std::vector<unsigned char> glyph;
    for (size_t i = 0; i < cps.size(); ++i) {
        const float shift = xs[i] - std::floor(xs[i]);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBoxSubpixel(&info, cps[i], scale, scale, shift, 0.0f, &x0, &y0, &x1, &y1);
        const int gw = x1 - x0, gh = y1 - y0;
        if (gw <= 0 || gh <= 0) continue;
        glyph.assign(size_t(gw) * size_t(gh), 0);
        stbtt_MakeCodepointBitmapSubpixel(&info, glyph.data(), gw, gh, gw, scale, scale, shift, 0.0f, cps[i]);
        const int ox = int(std::floor(xs[i])) + x0 + 1, oy = base + y0 + 1;
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                const int tx = ox + x, ty = oy + y;
                if (tx < 0 || ty < 0 || tx >= m.w || ty >= m.h) continue;
                float& d = m.v[size_t(ty) * size_t(m.w) + size_t(tx)];
                d = std::max(d, glyph[size_t(y) * size_t(gw) + size_t(x)] / 255.0f);
            }
    }
    return m;
}

class WatermarkNode : public Node {
public:
    enum { Text, Font, Size, Anchor, InsetX, InsetY, Opacity, Color, Shadow };
    REFRACTORY_NODE({"xform.watermark", "Watermark", "Transform",
                  {{"Image", PinType::Image}, {"Logo", PinType::Image}},
                  {{"Image", PinType::Image}},
                  {ParamDesc::Text("Text", "\xC2\xA9 Your Name"),
                   ParamDesc::Enum("Font", 0, {"Sans", "Sans Bold", "Serif", "Monospace"}),
                   ParamDesc::Float("Size", 4.0f, 0.5f, 50.0f),
                   ParamDesc::Enum("Anchor", 8, {"Top Left", "Top", "Top Right", "Left", "Center", "Right", "Bottom Left",
                                                 "Bottom", "Bottom Right"}),
                   ParamDesc::Float("Inset X", 3.0f, 0.0f, 50.0f), ParamDesc::Float("Inset Y", 3.0f, 0.0f, 50.0f),
                   ParamDesc::Float("Opacity", 0.7f, 0.0f, 1.0f), ParamDesc::Color("Color", 1.0f, 1.0f, 1.0f),
                   ParamDesc::Float("Shadow", 0.4f, 0.0f, 1.0f)}})
    // Per pixel at global positions: the mark is laid out in the whole frame.
    int roiPadding(const EvalContext&) const override { return 0; }

    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override {
        ImagePtr src = toImage(in[0], 0, 0);
        if (!src) return;
        const PixelFrame f = frameOf(ctx, src->w, src->h);
        const float shortEdge = float(std::min(f.fullW, f.fullH));
        const float px = paramF(Size) / 100.0f * shortEdge;
        ImagePtr logo = in[1].empty() ? nullptr : toImage(in[1], 0, 0);
        if (logo && (logo->w <= 0 || logo->h <= 0)) logo = nullptr;

        // The mark's coverage and colour, at its size in the frame.
        Mask mark;
        std::vector<float> rgb;  // the logo's colours (else the Color param)
        if (logo) {
            const float k = px * 2.5f / float(logo->h);  // a logo is a few text lines tall
            mark.w = std::max(1, int(std::lround(logo->w * k))), mark.h = std::max(1, int(std::lround(logo->h * k)));
            if (double(mark.w) * mark.h > 64e6) mark = Mask{};
            mark.v.assign(size_t(mark.w) * size_t(mark.h), 0.0f);
            rgb.assign(mark.v.size() * 3, 0.0f);
            parallelFor(mark.h, [&](int y) {
                for (int x = 0; x < mark.w; ++x) {
                    float p[4];
                    imageops::sampleBilinear(*logo, (x + 0.5f) / k, (y + 0.5f) / k, p);
                    const size_t i = size_t(y) * size_t(mark.w) + size_t(x);
                    mark.v[i] = std::clamp(p[3], 0.0f, 1.0f);
                    for (int c = 0; c < 3; ++c) rgb[i * 3 + size_t(c)] = p[c];
                }
            });
        } else {
            mark = renderText(params[Text].is_string() ? params[Text].get<std::string>() : std::string(), paramI(Font), px);
        }
        if (mark.v.empty()) {
            out[0] = Value(src);
            return;
        }
        // Placement in the frame: an anchor and an inset from the edges.
        const int anchor = std::clamp(paramI(Anchor), 0, 8), ax = anchor % 3, ay = anchor / 3;
        const float ix = paramF(InsetX) / 100.0f * shortEdge, iy = paramF(InsetY) / 100.0f * shortEdge;
        const int mx = int(std::lround(ax == 0 ? ix : ax == 2 ? f.fullW - ix - mark.w : (f.fullW - mark.w) * 0.5f));
        const int my = int(std::lround(ay == 0 ? iy : ay == 2 ? f.fullH - iy - mark.h : (f.fullH - mark.h) * 0.5f));

        // A soft drop shadow, offset down and right: the mark blurred, in a padded box at (sx0, sy0).
        const float shadowAmount = paramF(Shadow);
        Mask shadow;
        int sx0 = 0, sy0 = 0;
        if (shadowAmount > 0.0f) {
            const int off = std::max(1, int(std::lround(px * 0.06f))), pad = int(std::ceil(px * 0.15f)) + 1;
            shadow.w = mark.w + 2 * pad, shadow.h = mark.h + 2 * pad;
            shadow.v.assign(size_t(shadow.w) * size_t(shadow.h), 0.0f);
            for (int y = 0; y < mark.h; ++y)
                for (int x = 0; x < mark.w; ++x)
                    shadow.v[size_t(y + pad) * size_t(shadow.w) + size_t(x + pad)] = mark.v[size_t(y) * size_t(mark.w) + size_t(x)];
            const float sigma = std::max(0.5f, px * 0.04f);
            imageops::blurChannel(shadow.v, shadow.w, shadow.h, sigma, sigma);
            sx0 = mx - pad + off, sy0 = my - pad + off;
        }

        const float opacity = paramF(Opacity);
        float col[3];
        paramC(Color, col);
        const bool lin = ctx.linear();
        out[0] = Value(ImagePtr(mapImage(*src, [&](int x, int y, const float* s, float* d) {
            const int gx = f.x0 + x, gy = f.y0 + y;
            for (int k = 0; k < 4; ++k) d[k] = s[k];
            if (!shadow.v.empty()) {
                const int tx = gx - sx0, ty = gy - sy0;
                if (tx >= 0 && ty >= 0 && tx < shadow.w && ty < shadow.h) {
                    const float a = shadow.v[size_t(ty) * size_t(shadow.w) + size_t(tx)] * shadowAmount * opacity;
                    for (int k = 0; k < 3; ++k) d[k] *= 1.0f - a;
                    d[3] = d[3] + a * (1.0f - d[3]);
                }
            }
            const int tx = gx - mx, ty = gy - my;
            if (tx < 0 || ty < 0 || tx >= mark.w || ty >= mark.h) return;
            const size_t i = size_t(ty) * size_t(mark.w) + size_t(tx);
            const float a = mark.v[i] * opacity;
            for (int k = 0; k < 3; ++k) {
                const float c = rgb.empty() ? col[k] : rgb[i * 3 + size_t(k)];
                d[k] = clampColor(lin, d[k] + (c - d[k]) * a);
            }
            d[3] = d[3] + a * (1.0f - d[3]);
        })));
    }
};

}  // namespace

void registerWatermarkNodes(NodeRegistry& r) { r.add<WatermarkNode>(); }
