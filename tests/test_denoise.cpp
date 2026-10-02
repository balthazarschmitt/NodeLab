// Denoise: wavelet noise reduction with Lightroom's Luminance / Detail / Color / Color Detail.
#include <doctest/doctest.h>

#include <cmath>
#include <random>

#include "graph/Graph.h"
#include "graph/NodeRegistry.h"

namespace {

constexpr float kYr = 0.2126f, kYg = 0.7152f, kYb = 0.0722f;

float luma(const float* p) { return kYr * p[0] + kYg * p[1] + kYb * p[2]; }

// A flat colour with Gaussian noise: `lumaSigma` on all three channels alike (grain), and
// `chromaSigma` on Cb/Cr with Y held constant (coloured speckles).
ImagePtr noisy(float r, float g, float b, float lumaSigma, float chromaSigma, int w = 128, int h = 128) {
    auto img = std::make_shared<Image>(w, h);
    std::mt19937 rng(1234);
    std::normal_distribution<float> n(0.0f, 1.0f);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        const float dy = lumaSigma * n(rng);
        const float cb = chromaSigma * n(rng), cr = chromaSigma * n(rng);
        float* p = img->pixel(i);
        // Rec.709 YCbCr offsets: R and B move with Cr and Cb, G compensates to keep Y.
        const float dr = 1.5748f * cr, db = 1.8556f * cb;
        const float dg = -(kYr * dr + kYb * db) / kYg;
        p[0] = r + dy + dr, p[1] = g + dy + dg, p[2] = b + dy + db, p[3] = 1.0f;
    }
    return img;
}

ImagePtr run(Graph& g, Node* n, const ImagePtr& src, bool linear) {
    std::vector<Value> in{Value(src)};
    std::vector<Value> out(1);
    EvalContext ctx;
    ctx.defaultW = src->w, ctx.defaultH = src->h;
    if (linear) ctx.colorManagement = ColorManagement::sceneLinear();
    n->evaluate(ctx, in, out);
    ImagePtr r = toImage(out[0], 0, 0);
    REQUIRE(r);
    (void)g;
    return r;
}

void set(Node* n, const char* name, float v) {
    const auto& ps = n->info().params;
    for (size_t i = 0; i < ps.size(); ++i)
        if (ps[i].name == name) {
            n->params[i] = v;
            return;
        }
    FAIL("no param " << name);
}

struct Stats {
    double mean = 0, sd = 0;
};

// Statistics of f(pixel) over the image, skipping a border so edge clamping doesn't count.
template <class F>
Stats stats(const Image& img, F f, int border = 8) {
    double s = 0, s2 = 0;
    size_t n = 0;
    for (int y = border; y < img.h - border; ++y)
        for (int x = border; x < img.w - border; ++x) {
            const double v = f(img.pixel(size_t(y) * img.w + x));
            s += v, s2 += v * v, ++n;
        }
    Stats st;
    st.mean = s / n;
    st.sd = std::sqrt(std::max(0.0, s2 / n - st.mean * st.mean));
    return st;
}

float cbOf(const float* p) { return (p[2] - luma(p)) / 1.8556f; }

}  // namespace

TEST_CASE("Denoise at zero strength returns the input unchanged") {
    Graph g;
    Node* n = g.addNode("filter.denoise");
    REQUIRE(n);
    const ImagePtr src = noisy(0.4f, 0.5f, 0.3f, 0.03f, 0.02f, 32, 24);
    for (bool linear : {false, true}) {
        const ImagePtr out = run(g, n, src, linear);
        CHECK(out->px == src->px);
    }
}

TEST_CASE("Denoise Luminance removes grain and keeps the mean") {
    Graph g;
    Node* n = g.addNode("filter.denoise");
    REQUIRE(n);
    set(n, "Luminance", 50);
    for (bool linear : {false, true}) {
        CAPTURE(linear);
        // Linear projects stabilise with a square root, so the same grain is ~1.2x stronger
        // there at 0.18; use matching amounts in the encoded domain.
        const float base = linear ? 0.18f : 0.45f;
        const ImagePtr src = noisy(base, base, base, linear ? 0.015f : 0.02f, 0.0f);
        const ImagePtr out = run(g, n, src, linear);
        const Stats a = stats(*src, luma), b = stats(*out, luma);
        CHECK(b.sd < a.sd / 3);
        CHECK(std::fabs(b.mean - a.mean) < 0.004);
    }
}

TEST_CASE("Denoise keeps a step edge sharp") {
    Graph g;
    Node* n = g.addNode("filter.denoise");
    REQUIRE(n);
    set(n, "Luminance", 100);
    set(n, "Color", 100);
    const int w = 64, h = 16;
    auto src = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = x < w / 2 ? 0.2f : 0.8f;
            float* p = src->pixel(size_t(y) * w + x);
            p[0] = p[1] = p[2] = v, p[3] = 1.0f;
        }
    const ImagePtr out = run(g, n, src, false);
    // 10-90% rise width along the middle row.
    const float* row = out->pixel(size_t(h / 2) * w);
    int lo = -1, hi = -1;
    for (int x = 0; x < w; ++x) {
        const float v = row[size_t(x) * 4];
        if (lo < 0 && v > 0.2f + 0.06f) lo = x;
        if (hi < 0 && v > 0.2f + 0.54f) hi = x;
    }
    REQUIRE(lo >= 0);
    REQUIRE(hi >= 0);
    CHECK(hi - lo < 3);
    // Far from the edge, the flat sides are untouched.
    CHECK(row[4 * 2] == doctest::Approx(0.2f).epsilon(0.002));
    CHECK(row[size_t(w - 3) * 4] == doctest::Approx(0.8f).epsilon(0.002));
}

TEST_CASE("Denoise Color removes coloured speckles without touching brightness") {
    Graph g;
    Node* n = g.addNode("filter.denoise");
    REQUIRE(n);
    set(n, "Color", 50);
    const ImagePtr src = noisy(0.5f, 0.45f, 0.4f, 0.0f, 0.02f);
    const ImagePtr out = run(g, n, src, false);
    const Stats a = stats(*src, cbOf), b = stats(*out, cbOf);
    CHECK(b.sd < a.sd / 3);
    CHECK(std::fabs(b.mean - a.mean) < 0.002);
    // Luminance 0: Y passes through.
    float worst = 0;
    for (size_t i = 0; i < size_t(src->w) * src->h; ++i)
        worst = std::max(worst, std::fabs(luma(out->pixel(i)) - luma(src->pixel(i))));
    CHECK(worst < 1e-4f);
}
