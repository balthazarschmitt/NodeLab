#include "nodes/color/AutoTone.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "core/ColorManagement.h"
#include "core/ColorMath.h"
#include "core/Value.h"
#include "graph/NodeRegistry.h"

namespace autotone {
namespace {

// A box-filtered copy at most `edge` pixels long: enough for tone statistics, and small enough to
// run Basic dozens of times while searching.
Image shrink(const Image& src, int edge) {
    const int step = std::max(1, (std::max(src.w, src.h) + edge - 1) / edge);
    const int w = std::max(1, src.w / step), h = std::max(1, src.h / step);
    Image out(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float sum[4] = {0, 0, 0, 0};
            int n = 0;
            for (int yy = y * step; yy < std::min(src.h, (y + 1) * step); ++yy)
                for (int xx = x * step; xx < std::min(src.w, (x + 1) * step); ++xx) {
                    const float* p = src.pixel(size_t(yy) * src.w + xx);
                    if (!(std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && std::isfinite(p[3]))) continue;
                    for (int k = 0; k < 4; ++k) sum[k] += p[k];
                    ++n;
                }
            float* d = out.pixel(size_t(y) * w + x);
            for (int k = 0; k < 4; ++k) d[k] = n ? sum[k] / n : 0.0f;
        }
    return out;
}

// Perceptual brightness (sRGB-encoded luminance) of the visible pixels, sorted.
std::vector<float> tones(const Image& img, bool linear) {
    std::vector<float> v;
    v.reserve(size_t(img.w) * img.h);
    for (size_t i = 0; i < size_t(img.w) * img.h; ++i) {
        const float* p = img.pixel(i);
        if (!(p[3] > 0.0f)) continue;
        const float y = luminance(p[0], p[1], p[2]);
        const float l = linear ? colormath::linearToSrgb(std::max(y, 0.0f)) : y;
        if (std::isfinite(l)) v.push_back(l);
    }
    std::sort(v.begin(), v.end());
    return v;
}

float percentile(const std::vector<float>& v, float q) {
    if (v.empty()) return 0.0f;
    return v[std::min(v.size() - 1, size_t(q * float(v.size() - 1) + 0.5f))];
}

// The value in lo..hi where an increasing f crosses target (bisection).
float solve(float lo, float hi, float target, const std::function<float(float)>& f) {
    if (f(lo) >= target) return lo;
    if (f(hi) <= target) return hi;
    for (int i = 0; i < 14; ++i) {
        const float mid = 0.5f * (lo + hi);
        (f(mid) < target ? lo : hi) = mid;
    }
    return 0.5f * (lo + hi);
}

}  // namespace

Settings compute(const Image& src, bool linear) {
    Settings s;
    if (src.w <= 0 || src.h <= 0) return s;
    auto small = std::make_shared<Image>(shrink(src, 256));
    std::unique_ptr<Node> basic = NodeRegistry::instance().create("color.basic");
    if (!basic) return s;

    EvalContext ctx;
    ctx.defaultW = small->w, ctx.defaultH = small->h;
    if (linear) ctx.colorManagement = ColorManagement::sceneLinear();
    // The tones the Basic node gives with the settings so far.
    auto run = [&]() {
        const auto& p = autotone::kBasicParams;
        const float v[6] = {s.exposure, s.contrast, s.highlights, s.shadows, s.whites, s.blacks};
        for (int i = 0; i < 6; ++i) basic->params[p[i]] = v[i];
        std::vector<Value> in(basic->info().inputs.size()), out(basic->info().outputs.size());
        in[0] = Value(ImagePtr(small));
        if (in.size() > 1) in[1] = Value(1.0f);
        basic->evaluate(ctx, in, out);
        const ImagePtr r = toImage(out[0], 0, 0);
        return r ? tones(*r, linear) : std::vector<float>{};
    };
    if (run().empty()) return s;

    // Exposure: the middle tone to a little under mid grey, as Lightroom brightens a dark photo.
    // Darkening stops where the brightest tones would fall well below white, since Whites can
    // only win back so much: a bright scene stays bright rather than turning grey.
    const auto exposureFor = [&](float target, float q) {
        return solve(-4.0f, 4.0f, target, [&](float e) {
            s.exposure = e;
            return percentile(run(), q);
        });
    };
    float exposure = exposureFor(0.42f, 0.5f);
    if (exposure < 0.0f) exposure = std::min(0.0f, std::max(exposure, exposureFor(0.92f, 0.995f)));
    s.exposure = std::round(exposure * 20.0f) / 20.0f;

    // Contrast toward a moderate spread; flat images get more, punchy ones a little less.
    std::vector<float> t = run();
    float mean = 0, var = 0;
    for (float l : t) mean += l;
    mean /= float(t.size());
    for (float l : t) var += (l - mean) * (l - mean);
    const float sd = std::sqrt(var / float(t.size()));
    s.contrast = std::round(std::clamp((0.21f - sd) * 250.0f, -25.0f, 35.0f));

    // Recover bright and lift dark areas by how much of the image sits there.
    t = run();
    const auto share = [&](auto pred) { return float(std::count_if(t.begin(), t.end(), pred)) / float(t.size()); };
    const float bright = share([](float l) { return l > 0.75f; }), dark = share([](float l) { return l < 0.2f; });
    const float top = percentile(t, 0.995f);
    const float wantHighlights = -std::clamp(bright * 250.0f + std::max(0.0f, percentile(t, 0.99f) - 0.9f) * 200.0f, 0.0f, 70.0f);
    s.shadows = std::round(std::clamp(dark * 250.0f, 0.0f, 70.0f));
    // Highlights should recover detail in bright areas, not grey the white point (Whites can't
    // always win that back): no further than lowering the top tones by a few percent.
    s.highlights = std::round(solve(wantHighlights, 0.0f, top - 0.03f, [&](float hl) {
        s.highlights = hl;
        return percentile(run(), 0.995f);
    }));

    // White and black points: the brightest and darkest half percent just short of clipping,
    // within the moderate range Lightroom's Auto keeps to.
    s.whites = std::round(solve(-70.0f, 70.0f, 0.97f, [&](float w) {
        s.whites = w;
        return percentile(run(), 0.995f);
    }));
    s.blacks = std::round(solve(-70.0f, 70.0f, 0.02f, [&](float b) {
        s.blacks = b;
        return percentile(run(), 0.005f);
    }));
    return s;
}

}  // namespace autotone
