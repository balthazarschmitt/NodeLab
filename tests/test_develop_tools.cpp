// darktable- and RawTherapee-style develop tools: Tone Equalizer, Color Equalizer, Film Negative,
// Capture Sharpening, Diffuse or Sharpen and content-aware Remove.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "core/ColorScience.h"
#include "graph/Graph.h"
// A private copy of stb_truetype (Watermark.cpp keeps its own static).
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

extern const char* const kFontRoboto;
extern const std::size_t kFontRobotoSize;
extern const char* const kFontCousine;
extern const std::size_t kFontCousineSize;

namespace {

// Left half `a`, right half `b` (grey, or a colour when given three values).
ImagePtr halves(int w, int h, const float a[3], const float b[3]) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img->pixel(size_t(y) * w + x);
            const float* c = x < w / 2 ? a : b;
            p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = 1.0f;
        }
    return img;
}

ImagePtr run(Node* n, const ImagePtr& img, bool linear) {
    std::vector<Value> in(n->info().inputs.size()), out(n->info().outputs.size());
    in[0] = Value(img);
    EvalContext ctx;
    ctx.defaultW = img->w, ctx.defaultH = img->h;
    ctx.colorManagement.linear = linear;
    n->evaluate(ctx, in, out);
    ImagePtr r = std::get<ImagePtr>(out[0].v);
    REQUIRE(r);
    return r;
}

}  // namespace

TEST_CASE("Tone Equalizer lifts the band a region's exposure falls in") {
    Graph g;
    Node* n = g.addNode("color.tone_equalizer");
    REQUIRE(n);
    const float dark[3] = {0.01f, 0.01f, 0.01f}, bright[3] = {0.5f, 0.5f, 0.5f};  // about -6.6 and -1 EV
    const ImagePtr img = halves(64, 32, dark, bright);
    for (float smoothing : {0.0f, 2.0f}) {
        CAPTURE(smoothing);
        n->params[10] = smoothing;
        n->params[1 + 1] = 1.0f;  // -7 EV: +1 stop
        ImagePtr r = run(n, img, true);
        CHECK(r->pixel(4)[0] > 0.01f * 1.6f);  // the shadows come up
        CHECK(r->pixel(60)[0] == doctest::Approx(0.5f).epsilon(0.01));  // the highlights don't
        CHECK(r->pixel(4)[0] == doctest::Approx(r->pixel(4)[2]));      // neutral stays neutral
        // Mask Exposure moves which band a region reads.
        n->params[12] = 3.0f;
        CHECK(run(n, img, true)->pixel(4)[0] < r->pixel(4)[0]);
        n->params[12] = 0.0f;
        n->params[2] = 0.0f;
    }
    // The mask shows dark regions darker than bright ones.
    n->params[14] = true;
    const ImagePtr m = run(n, img, false);
    CHECK(m->pixel(4)[0] < m->pixel(60)[0]);
    n->params[14] = false;
    // A band's slider is the gain at its exposure: -3 EV at -1 makes 0.125 a stop darker.
    const float mid[3] = {0.125f, 0.125f, 0.125f};
    n->params[10] = 0.0f;
    n->params[1 + 5] = -1.0f;
    CHECK(run(n, halves(4, 2, mid, mid), true)->pixel(0)[0] == doctest::Approx(0.0625f).epsilon(1e-3));
    n->params[1 + 5] = 0.0f;
    // All bands at zero: unchanged (legacy round trip through linear light).
    const ImagePtr same = run(n, img, false);
    CHECK(same->pixel(60)[0] == doctest::Approx(0.5f).epsilon(1e-4));
}

TEST_CASE("Color Equalizer changes the hue it's set for and leaves greys") {
    Graph g;
    Node* n = g.addNode("color.color_equalizer");
    REQUIRE(n);
    const float red[3] = {0.6f, 0.05f, 0.05f}, grey[3] = {0.3f, 0.3f, 0.3f};
    const ImagePtr img = halves(8, 2, red, grey);
    const auto chroma = [](const float* p) {
        float lab[3];
        colorsci::rgbToOklab(p, lab);
        return std::hypot(lab[1], lab[2]);
    };
    // Red Saturation -100: the red loses (almost) all colour; grey is untouched.
    n->params[9] = -100.0f;
    ImagePtr r = run(n, img, true);
    CHECK(chroma(r->pixel(0)) < 0.25f * chroma(img->pixel(0)));
    CHECK(r->pixel(6)[0] == doctest::Approx(0.3f).epsilon(1e-4));
    CHECK(r->pixel(6)[1] == doctest::Approx(0.3f).epsilon(1e-4));
    // Blue's sliders don't reach red.
    n->params[9] = 0.0f;
    n->params[9 + 5] = -100.0f;
    r = run(n, img, true);
    CHECK(chroma(r->pixel(0)) > 0.9f * chroma(img->pixel(0)));
    // Red Brightness up makes it lighter; Red Hue moves it towards orange (more green).
    n->params[9 + 5] = 0.0f;
    n->params[17] = 100.0f;
    CHECK(luminance(run(n, img, true)->pixel(0)[0], 0, 0) > 0.0f);
    float labIn[3], labOut[3];
    colorsci::rgbToOklab(img->pixel(0), labIn);
    colorsci::rgbToOklab(run(n, img, true)->pixel(0), labOut);
    CHECK(labOut[0] > labIn[0] + 0.02f);
    n->params[17] = 0.0f;
    n->params[1] = 30.0f;
    r = run(n, img, false);
    CHECK(r->pixel(0)[1] > img->pixel(0)[1] + 0.02f);
}

TEST_CASE("Film Negative turns a scanned negative positive") {
    Graph g;
    Node* n = g.addNode("color.film_negative");
    REQUIRE(n);
    // A linear project: the film base (clear film) is the orange mask; a dense patch (a highlight
    // in the scene) lets through a tenth as much light.
    const float base[3] = {0.75f, 0.35f, 0.15f};
    const float dense[3] = {0.075f, 0.035f, 0.015f};
    const ImagePtr img = halves(4, 2, base, dense);
    ImagePtr r = run(n, img, true);
    // Clear film prints black, dense film bright, and the mask's colour cancels: both neutral.
    CHECK(r->pixel(0)[0] < 0.01f);
    CHECK(r->pixel(3)[0] > 20 * r->pixel(0)[0]);
    CHECK(r->pixel(3)[0] == doctest::Approx(r->pixel(3)[1]).epsilon(1e-3));
    CHECK(r->pixel(3)[1] == doctest::Approx(r->pixel(3)[2]).epsilon(1e-3));
    // Print Exposure brightens by stops; D Max at the patch's density makes it white.
    n->params[6] = 1.0f;
    CHECK(run(n, img, true)->pixel(3)[0] == doctest::Approx(2 * r->pixel(3)[0]).epsilon(1e-3));
    n->params[6] = 0.0f;
    n->params[2] = 1.0f;
    CHECK(run(n, img, true)->pixel(3)[0] == doctest::Approx(1.0f).epsilon(1e-3));
    // Black & White: one density for all channels, even off the film's colour.
    const float odd[3] = {0.05f, 0.03f, 0.015f};
    n->params[0] = 1;
    r = run(n, halves(4, 2, odd, odd), false);
    CHECK(r->pixel(0)[0] == doctest::Approx(r->pixel(0)[2]));
}

namespace {

// A vertical edge from 0.1 to 0.6 (linear), blurred by a Gaussian of `sigma` pixels.
ImagePtr softEdge(int w, int h, float sigma) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float t = 0.5f * (1.0f + std::erf((x + 0.5f - w * 0.5f) / (sigma * std::sqrt(2.0f))));
            float* p = img->pixel(size_t(y) * w + x);
            p[0] = p[1] = p[2] = 0.1f + 0.5f * t, p[3] = 1.0f;
        }
    return img;
}

// The steepest step between neighbouring pixels along the middle row.
float steepest(const Image& img) {
    float best = 0;
    const int y = img.h / 2;
    for (int x = 1; x < img.w; ++x) best = std::max(best, img.pixel(size_t(y) * img.w + x)[0] - img.pixel(size_t(y) * img.w + x - 1)[0]);
    return best;
}

double variance(const Image& img) {
    double s = 0, s2 = 0;
    const size_t n = size_t(img.w) * img.h;
    for (size_t i = 0; i < n; ++i) s += img.pixel(i)[1], s2 += double(img.pixel(i)[1]) * img.pixel(i)[1];
    return s2 / n - (s / n) * (s / n);
}

}  // namespace

TEST_CASE("Capture Sharpening undoes a small Gaussian blur") {
    Graph g;
    Node* n = g.addNode("filter.capture_sharpen");
    REQUIRE(n);
    const ImagePtr img = softEdge(32, 8, 1.0f);
    n->params[0] = 1.0f;
    const ImagePtr r = run(n, img, true);
    CHECK(steepest(*r) > 1.3f * steepest(*img));
    // Far from the edge, flat areas don't change.
    CHECK(r->pixel(2)[0] == doctest::Approx(img->pixel(2)[0]).epsilon(1e-3));
    // Amount 0, or a preview too small for the blur to matter: unchanged.
    n->params[3] = 0.0f;
    CHECK(steepest(*run(n, img, true)) == doctest::Approx(steepest(*img)).epsilon(1e-4));
}

TEST_CASE("Diffuse or Sharpen adds detail above zero and removes it below") {
    Graph g;
    Node* n = g.addNode("filter.diffuse");
    REQUIRE(n);
    // Mid-grey with a fine checker of +-0.05.
    auto img = std::make_shared<Image>(32, 32);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            float* p = img->pixel(size_t(y) * 32 + x);
            p[0] = p[1] = p[2] = 0.4f + (((x / 2 + y / 2) & 1) ? 0.05f : -0.05f), p[3] = 1.0f;
        }
    n->params[1] = 2.0f;   // radius at the checker's scale
    n->params[5] = 0.0f;   // no noise threshold
    n->params[4] = 0.0f;   // nor edge sensitivity
    n->params[0] = 50.0f;
    const double v0 = variance(*img);
    CHECK(variance(*run(n, img, false)) > 1.3 * v0);
    n->params[0] = -100.0f;
    CHECK(variance(*run(n, img, false)) < 0.5 * v0);
    // Edge Sensitivity keeps a strong edge while diffusing.
    const ImagePtr edge = softEdge(32, 8, 0.5f);
    n->params[4] = 100.0f;
    CHECK(steepest(*run(n, edge, false)) > 0.7f * steepest(*edge));
}

TEST_CASE("Remove fills a masked area from the texture around it") {
    Graph g;
    Node* n = g.addNode("filter.remove");
    REQUIRE(n);
    n->params[0] = 0.0f;  // no Grow: the hole is exactly the mask
    // Vertical stripes, 4 px dark and 4 px light, with a square of red to remove.
    const int w = 96, h = 96;
    auto img = std::make_shared<Image>(w, h);
    auto mask = std::make_shared<Channel>(Channel::makeSized(w, h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img->pixel(size_t(y) * w + x);
            const bool in = x >= 36 && x < 60 && y >= 36 && y < 60;
            const float v = (x / 4) % 2 ? 0.8f : 0.2f;
            p[0] = in ? 1.0f : v, p[1] = in ? 0.0f : v, p[2] = in ? 0.0f : v, p[3] = 1.0f;
            mask->data[size_t(y) * w + x] = in ? 1.0f : 0.0f;
        }
    std::vector<Value> in(2), out(1);
    in[0] = Value(ImagePtr(img));
    in[1] = Value(ChannelPtr(mask));
    EvalContext ctx;
    ctx.defaultW = w, ctx.defaultH = h;
    n->evaluate(ctx, in, out);
    const ImagePtr r = std::get<ImagePtr>(out[0].v);
    REQUIRE(r);
    // No red is left, and the hole's pixels are grey and mostly at the stripes' two levels.
    int onLevel = 0, total = 0;
    for (int y = 36; y < 60; ++y)
        for (int x = 36; x < 60; ++x) {
            const float* p = r->pixel(size_t(y) * w + x);
            CHECK(p[0] - p[1] < 0.05f);
            onLevel += std::min(std::abs(p[1] - 0.2f), std::abs(p[1] - 0.8f)) < 0.15f;
            ++total;
        }
    CHECK(onLevel > total * 3 / 4);
    // Outside the mask nothing changes; a deterministic result.
    CHECK(r->pixel(5)[0] == img->pixel(5)[0]);
    std::vector<Value> out2(1);
    n->evaluate(ctx, in, out2);
    const ImagePtr r2 = std::get<ImagePtr>(out2[0].v);
    CHECK(std::equal(r->px.begin(), r->px.end(), r2->px.begin()));
}

TEST_CASE("Watermark's built-in fonts load") {
    // Used when Windows' fonts are missing; stb_truetype must accept them.
    for (const auto& [data, size] : {std::pair{kFontRoboto, kFontRobotoSize}, std::pair{kFontCousine, kFontCousineSize}}) {
        REQUIRE(size > 10000);
        const auto* p = reinterpret_cast<const unsigned char*>(data);
        stbtt_fontinfo info;
        CHECK(stbtt_InitFont(&info, p, stbtt_GetFontOffsetForIndex(p, 0)));
        CHECK(stbtt_FindGlyphIndex(&info, 'W') > 0);
    }
}

TEST_CASE("Watermark draws text at its anchor, or a logo, at a size relative to the image") {
    Graph g;
    Node* n = g.addNode("xform.watermark");
    REQUIRE(n);
    const float grey[3] = {0.2f, 0.2f, 0.2f};
    const ImagePtr img = halves(400, 300, grey, grey);
    n->params[8] = 0.0f;  // no shadow: only the text changes pixels
    n->params[6] = 1.0f;  // opaque
    n->params[0] = "WATERMARK";
    auto bounds = [&](const Image& r, int& x0, int& y0, int& x1, int& y1) {
        x0 = r.w, y0 = r.h, x1 = -1, y1 = -1;
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x)
                if (r.pixel(size_t(y) * r.w + x)[0] > 0.5f)
                    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    };
    ImagePtr r = run(n, img, true);
    int x0, y0, x1, y1;
    bounds(*r, x0, y0, x1, y1);
    REQUIRE(x1 >= 0);  // a system font, or the built-in one
    // Bottom right, inset 3% of the short edge (9 px), about 4% (12 px) tall.
    CHECK(x1 > 400 - 9 - 8);
    CHECK(x1 <= 400 - 9);
    CHECK(y1 > 300 - 9 - 8);
    CHECK(y1 <= 300 - 9);
    CHECK(y1 - y0 >= 4);  // capitals: about half the line height
    CHECK(y1 - y0 <= 16);
    CHECK(r->pixel(0)[0] == doctest::Approx(0.2f));  // the rest is untouched

    // Top left; twice the size doubles the height.
    n->params[3] = 0;
    n->params[2] = 8.0f;
    ImagePtr big = run(n, img, true);
    int bx0, by0, bx1, by1;
    bounds(*big, bx0, by0, bx1, by1);
    CHECK(bx0 >= 9);
    CHECK(bx0 < 9 + 6);
    CHECK(by0 >= 9);
    CHECK(float(by1 - by0) == doctest::Approx(2.0f * float(y1 - y0)).epsilon(0.25));

    // The same mark at half the resolution lands in the same place, scaled.
    const ImagePtr half = halves(200, 150, grey, grey);
    ImagePtr small = run(n, half, true);
    int sx0, sy0, sx1, sy1;
    bounds(*small, sx0, sy0, sx1, sy1);
    CHECK(std::abs(sx1 * 2 - bx1) <= 3);
    CHECK(std::abs(sy1 * 2 - by1) <= 3);

    // Empty text leaves the image as it was.
    n->params[0] = "";
    CHECK(run(n, img, true)->px == img->px);

    // A logo replaces the text, keeping its colours and alpha.
    n->params[0] = "ignored";
    n->params[3] = 4;  // centre
    auto logo = std::make_shared<Image>(20, 10);
    for (size_t i = 0; i < logo->pixelCount(); ++i) {
        float* p = logo->pixel(i);
        p[0] = 0.0f, p[1] = 1.0f, p[2] = 0.0f, p[3] = 1.0f;
    }
    std::vector<Value> in(2), out(1);
    in[0] = Value(img), in[1] = Value(ImagePtr(logo));
    EvalContext ctx;
    ctx.colorManagement.linear = true;
    n->evaluate(ctx, in, out);
    const ImagePtr lr = std::get<ImagePtr>(out[0].v);
    const float* c = lr->pixel(150 * 400 + 200);
    CHECK(c[1] == doctest::Approx(1.0f));
    CHECK(c[0] == doctest::Approx(0.0f));
}
