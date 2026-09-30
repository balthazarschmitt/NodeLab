// Scene-linear develop maths (Basic, Color Mixer, Color Grading): CAT16 white balance, the tone
// equalizer, log-space contrast and Oklch colour work.
#include <doctest/doctest.h>

#include <cmath>

#include "core/ColorScience.h"
#include "graph/Graph.h"
#include "graph/NodeRegistry.h"

namespace {

ImagePtr solid(float r, float g, float b, int w = 8, int h = 8) {
    auto img = std::make_shared<Image>(w, h);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        float* p = img->pixel(i);
        p[0] = r, p[1] = g, p[2] = b, p[3] = 1.0f;
    }
    return img;
}

// Grey levels from -10 to +2 stops (relative to 1.0), left to right, with a colour tint.
ImagePtr evRamp(int w = 64, int h = 4) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = std::exp2(-10.0f + 12.0f * x / (w - 1));
            float* p = img->pixel(size_t(y) * w + x);
            p[0] = v, p[1] = v * 0.8f, p[2] = v * 0.6f, p[3] = 1.0f;
        }
    return img;
}

ImagePtr runLinear(Node& n, const ImagePtr& src) {
    const NodeInfo& info = n.info();
    std::vector<Value> in(info.inputs.size());
    in[0] = Value(src);
    for (size_t i = 1; i < info.inputs.size(); ++i)
        if (info.inputs[i].fallbackParam >= 0) in[i] = Value(n.paramF(info.inputs[i].fallbackParam));
    std::vector<Value> out(info.outputs.size());
    EvalContext ctx;
    ctx.defaultW = src->w, ctx.defaultH = src->h;
    ctx.colorManagement = ColorManagement::sceneLinear();
    n.evaluate(ctx, in, out);
    ImagePtr r = toImage(out[0], 0, 0);
    REQUIRE(r);
    return r;
}

float maxDiff(const Image& a, const Image& b) {
    float m = 0;
    for (size_t i = 0; i < a.px.size(); ++i) m = std::max(m, std::fabs(a.px[i] - b.px[i]));
    return m;
}

float lum(const float* p) { return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; }

struct Fixture {
    Graph g;
    Node* add(const char* type) {
        Node* n = g.addNode(type);
        REQUIRE(n);
        return n;
    }
    // Basic's params by name, as in the Inspector.
    void set(Node* n, const char* name, float v) {
        const auto& ps = n->info().params;
        for (size_t i = 0; i < ps.size(); ++i)
            if (ps[i].name == name) {
                n->params[i] = v;
                return;
            }
        FAIL("no param " << name);
    }
};

}  // namespace

TEST_CASE("CAT16 white balance: identity at zero, neutralises its own light, keeps white's brightness") {
    colorsci::Mat3 m;
    colorsci::whiteBalanceMatrix(0, 0, m);
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) CHECK(m[r][c] == doctest::Approx(r == c ? 1.0f : 0.0f).epsilon(1e-5));

    for (float temp : {-1.0f, -0.4f, 0.5f, 1.0f})
        for (float tint : {-1.0f, 0.0f, 0.7f}) {
            CAPTURE(temp);
            CAPTURE(tint);
            float light[3], out[3];
            colorsci::whiteBalanceSource(temp, tint, light);
            CHECK(lum(light) == doctest::Approx(1.0f).epsilon(1e-3));
            colorsci::whiteBalanceMatrix(temp, tint, m);
            colorsci::mul(m, light, out);
            for (float v : out) CHECK(v == doctest::Approx(1.0f).epsilon(2e-3));
        }

    // +Temperature assumes a bluer light, so a grey comes out warm; +Tint comes out magenta.
    const float grey[3] = {0.18f, 0.18f, 0.18f};
    float out[3];
    colorsci::whiteBalanceMatrix(0.5f, 0, m);
    colorsci::mul(m, grey, out);
    CHECK(out[0] > out[2]);
    colorsci::whiteBalanceMatrix(0, 0.5f, m);
    colorsci::mul(m, grey, out);
    CHECK(out[1] < out[0]);
    CHECK(out[1] < out[2]);
}

TEST_CASE("Oklab round-trips and gamut compression keeps luminance") {
    const float c[3] = {0.8f, 0.2f, 0.05f};
    float lab[3], back[3];
    colorsci::rgbToOklab(c, lab);
    colorsci::oklabToRgb(lab, back);
    for (int k = 0; k < 3; ++k) CHECK(back[k] == doctest::Approx(c[k]).epsilon(1e-4));
    const float white[3] = {1, 1, 1};
    colorsci::rgbToOklab(white, lab);
    CHECK(lab[0] == doctest::Approx(1.0f).epsilon(1e-4));
    CHECK(std::fabs(lab[1]) < 1e-4f);

    float out[3] = {0.9f, -0.1f, 0.2f};
    const float y = lum(out);
    colorsci::compressToGamut(out);
    CHECK(std::min({out[0], out[1], out[2]}) >= 0.0f);
    CHECK(lum(out) == doctest::Approx(y).epsilon(1e-4));
    CHECK(out[0] > out[2]);  // still reddish
}

TEST_CASE_FIXTURE(Fixture, "Basic in scene-linear: defaults are identity, Exposure multiplies without clipping") {
    Node* n = add("color.basic");
    auto src = evRamp();
    CHECK(maxDiff(*runLinear(*n, src), *src) < 1e-6f);

    set(n, "Exposure", 1.0f);
    auto r = runLinear(*n, src);
    for (size_t i = 0; i < src->px.size(); i += 4) CHECK(r->px[i] == doctest::Approx(src->px[i] * 2.0f));
    CHECK(r->px[src->px.size() - 4] > 7.9f);  // +2 EV input -> 8.0, not clipped
}

TEST_CASE_FIXTURE(Fixture, "Basic tone sliders keep RGB ratios and stay monotonic") {
    auto src = evRamp();
    for (const char* name : {"Contrast", "Highlights", "Shadows", "Whites", "Blacks"})
        for (float v : {-100.0f, 100.0f}) {
            CAPTURE(name);
            CAPTURE(v);
            Node* n = add("color.basic");
            set(n, name, v);
            auto r = runLinear(*n, src);
            float prev = -1.0f;
            bool changed = false;
            for (int x = 0; x < src->w; ++x) {
                const float* s = src->pixel(size_t(x) + src->w);
                const float* d = r->pixel(size_t(x) + src->w);
                CHECK(d[1] / d[0] == doctest::Approx(s[1] / s[0]).epsilon(1e-3));  // hue kept
                CHECK(d[2] / d[0] == doctest::Approx(s[2] / s[0]).epsilon(1e-3));
                CHECK(d[0] >= prev);
                changed |= std::fabs(d[0] - s[0]) > 1e-4f * s[0] + 1e-7f;
                prev = d[0];
            }
            CHECK(changed);
        }
}

TEST_CASE_FIXTURE(Fixture, "Highlights recovers values above white; Shadows lifts dark regions") {
    Node* n = add("color.basic");
    set(n, "Highlights", -100.0f);
    auto bright = solid(2.0f, 1.6f, 1.2f);
    CHECK(runLinear(*n, bright)->pixel(0)[0] < 1.0f);  // back under white

    Node* s = add("color.basic");
    set(s, "Shadows", 100.0f);
    auto dark = solid(0.01f, 0.01f, 0.01f);
    CHECK(runLinear(*s, dark)->pixel(0)[0] > 0.03f);
    auto mid = solid(0.5f, 0.5f, 0.5f);
    CHECK(runLinear(*s, mid)->pixel(0)[0] == doctest::Approx(0.5f).epsilon(0.02));  // bright areas stay
    auto black = solid(0, 0, 0);
    CHECK(runLinear(*s, black)->pixel(0)[0] == 0.0f);  // gains never lift pure black
}

TEST_CASE_FIXTURE(Fixture, "Vibrance and Saturation work on chroma only") {
    auto src = std::make_shared<Image>(3, 1);
    const float cols[3][3] = {{0.5f, 0.3f, 0.2f}, {0.1f, 0.3f, 0.12f}, {0.2f, 0.2f, 0.25f}};
    for (int i = 0; i < 3; ++i) {
        float* p = src->pixel(size_t(i));
        p[0] = cols[i][0], p[1] = cols[i][1], p[2] = cols[i][2], p[3] = 1;
    }
    for (const char* name : {"Vibrance", "Saturation"}) {
        CAPTURE(name);
        Node* n = add("color.basic");
        set(n, name, 40.0f);
        auto r = runLinear(*n, src);
        for (int i = 0; i < 3; ++i) {
            float a[3], b[3];
            colorsci::rgbToOklab(src->pixel(size_t(i)), a);
            colorsci::rgbToOklab(r->pixel(size_t(i)), b);
            CHECK(b[0] == doctest::Approx(a[0]).epsilon(1e-3));  // lightness unchanged
            CHECK(std::hypot(b[1], b[2]) > std::hypot(a[1], a[2]));
        }
    }
    Node* n = add("color.basic");
    set(n, "Saturation", -100.0f);
    auto r = runLinear(*n, src);
    const float* p = r->pixel(0);
    CHECK(std::fabs(p[0] - p[1]) < 1e-3f);
    CHECK(std::fabs(p[2] - p[1]) < 1e-3f);
}

TEST_CASE_FIXTURE(Fixture, "Color Mixer in scene-linear: bands select their colours, greys untouched") {
    Node* n = add("color.color_mixer");
    auto src = std::make_shared<Image>(3, 1);
    const float cols[3][3] = {{0.05f, 0.1f, 0.6f}, {0.6f, 0.05f, 0.03f}, {0.3f, 0.3f, 0.3f}};
    for (int i = 0; i < 3; ++i) {
        float* p = src->pixel(size_t(i));
        p[0] = cols[i][0], p[1] = cols[i][1], p[2] = cols[i][2], p[3] = 1;
    }
    CHECK(maxDiff(*runLinear(*n, src), *src) == 0.0f);

    set(n, "Blue Saturation", -100.0f);
    set(n, "Blue Luminance", 50.0f);
    auto r = runLinear(*n, src);
    const float* blue = r->pixel(0);
    CHECK(std::fabs(blue[2] - blue[0]) < 0.3f * std::fabs(cols[0][2] - cols[0][0]));  // desaturated
    for (int k = 0; k < 3; ++k) {
        CHECK(r->pixel(1)[k] == doctest::Approx(cols[1][k]).epsilon(1e-3));  // red untouched
        CHECK(r->pixel(2)[k] == doctest::Approx(cols[2][k]).epsilon(1e-3));  // grey untouched
    }
}

TEST_CASE_FIXTURE(Fixture, "Color Grading in scene-linear: identity at zero, black stays neutral") {
    Node* n = add("color.color_grading");
    auto src = evRamp();
    CHECK(maxDiff(*runLinear(*n, src), *src) == 0.0f);
    set(n, "Shadows Saturation", 60.0f);  // blue-ish by default
    set(n, "Highlights Saturation", 60.0f);
    auto r = runLinear(*n, src);
    const float* dark = r->pixel(8);
    CHECK(dark[2] / dark[0] > src->pixel(8)[2] / src->pixel(8)[0]);  // shadows toward blue
    auto black = solid(0, 0, 0);
    auto blackOut = runLinear(*n, black);  // keep the image alive while reading its pixels
    const float* b = blackOut->pixel(0);
    CHECK(std::max({b[0], b[1], b[2]}) < 1e-4f);
}
