// Photo Merge nodes: HDR Merge and Panorama Merge on synthetic pictures cut from a known scene.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "graph/Graph.h"
#include "graph/NodeRegistry.h"

namespace {

float hash(int x, int y, int seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + uint32_t(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffffff) / float(0xffffff);
}

float valueNoise(float x, float y, float cell, int seed) {
    const float fx = x / cell, fy = y / cell;
    const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
    const float tx = fx - float(ix), ty = fy - float(iy);
    const float a = hash(ix, iy, seed), b = hash(ix + 1, iy, seed), c = hash(ix, iy + 1, seed), d = hash(ix + 1, iy + 1, seed);
    return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
}

// A scene with detail at several scales and hard-edged blocks (corners to match), in linear
// light from 0.01 to about 5: more range than one exposure holds.
std::shared_ptr<Image> scene(int w, int h) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float f = 0.5f * valueNoise(float(x), float(y), 40, 1) + 0.3f * valueNoise(float(x), float(y), 9, 2);
            f += hash(x / 12, y / 12, 3) > 0.8f ? 0.2f : 0.0f;
            const float L = 0.01f * std::exp2(9.0f * std::clamp(f, 0.0f, 1.0f));
            float* p = img->pixel(size_t(y) * size_t(w) + size_t(x));
            p[0] = L, p[1] = L * (0.8f + 0.2f * hash(x / 16, y / 16, 4)), p[2] = L * 0.9f, p[3] = 1.0f;
        }
    return img;
}

ImagePtr crop(const Image& src, int x0, int y0, int w, int h, float gain, bool clip) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float* s = src.pixel(size_t(y + y0) * size_t(src.w) + size_t(x + x0));
            float* d = img->pixel(size_t(y) * size_t(w) + size_t(x));
            for (int c = 0; c < 3; ++c) d[c] = clip ? std::clamp(s[c] * gain, 0.0f, 1.0f) : s[c] * gain;
            d[3] = s[3];
        }
    return img;
}

ImagePtr run(Node* n, const std::vector<ImagePtr>& imgs, bool linear) {
    n->loadExtra({{"images", int(imgs.size()) + 1}});  // with the empty pin at the end, as in the editor
    REQUIRE(int(n->info().inputs.size()) == int(imgs.size()) + 1);
    std::vector<Value> in(imgs.size() + 1), out(1);
    for (size_t i = 0; i < imgs.size(); ++i) in[i] = Value(imgs[i]);
    EvalContext ctx;
    if (linear) ctx.colorManagement = ColorManagement::sceneLinear();
    n->evaluate(ctx, in, out);
    ImagePtr r = toImage(out[0], 0, 0);
    REQUIRE(r);
    return r;
}

// Mean relative error of the red channel against the scene seen at offset (ox, oy), away from
// the borders (where shifted frames have no data).
double relError(const Image& got, const Image& ref, int ox, int oy, int margin) {
    double e = 0;
    int n = 0;
    for (int y = margin; y < got.h - margin; ++y)
        for (int x = margin; x < got.w - margin; ++x) {
            const float a = got.pixel(size_t(y) * size_t(got.w) + size_t(x))[0];
            const float b = ref.pixel(size_t(y + oy) * size_t(ref.w) + size_t(x + ox))[0];
            e += std::abs(a - b) / b, ++n;
        }
    return e / n;
}

}  // namespace

TEST_CASE("HDR Merge aligns brackets and recovers the scene's range") {
    const int w = 256, h = 192, pad = 12;
    ImagePtr s = scene(w + 2 * pad, h + 2 * pad);
    // Three exposures two stops apart, each handheld a few pixels off, clipped at 1.
    ImagePtr mid = crop(*s, pad, pad, w, h, 1.0f, true);
    ImagePtr dark = crop(*s, pad + 3, pad - 2, w, h, 0.25f, true);
    ImagePtr bright = crop(*s, pad - 4, pad + 1, w, h, 4.0f, true);
    auto n = NodeRegistry::instance().create("math.hdr_merge");
    REQUIRE(n);

    ImagePtr r = run(n.get(), {dark, mid, bright}, true);
    CHECK(r->w == w);
    CHECK(r->h == h);
    // At the middle frame's exposure, with the highlights it clipped brought back.
    CHECK(relError(*r, *s, pad, pad, 8) < 0.03);
    float maxR = 0;
    for (size_t i = 0; i < r->pixelCount(); ++i) maxR = std::max(maxR, r->pixel(i)[0]);
    CHECK(maxR > 3.0f);

    // Without alignment the shifts show.
    n->params[0] = false;
    ImagePtr un = run(n.get(), {dark, mid, bright}, true);
    CHECK(relError(*un, *s, pad, pad, 8) > 2 * relError(*r, *s, pad, pad, 8));

    // Deghost keeps something that moved in one frame out of the result.
    n->params[0] = true;
    n->params[1] = 3;
    auto moved = std::make_shared<Image>(*bright);
    for (int y = 60; y < 100; ++y)
        for (int x = 60; x < 100; ++x) {
            float* p = moved->pixel(size_t(y) * size_t(w) + size_t(x));
            p[0] = p[1] = p[2] = 0.15f;  // well exposed, so the plain merge takes it in
        }
    ImagePtr dg = run(n.get(), {dark, mid, moved}, true);
    n->params[1] = 0;
    ImagePtr ghosty = run(n.get(), {dark, mid, moved}, true);
    auto areaError = [&](const Image& img) {
        double e = 0;
        for (int y = 70; y < 90; ++y)
            for (int x = 70; x < 90; ++x) {
                const float a = img.pixel(size_t(y) * size_t(w) + size_t(x))[0];
                const float b = s->pixel(size_t(y + pad) * size_t(s->w) + size_t(x + pad))[0];
                e += std::abs(a - b) / b;
            }
        return e / 400;
    };
    CHECK(areaError(*dg) < 0.1);
    CHECK(areaError(*ghosty) > 2 * areaError(*dg));

    // Legacy projects: sRGB-encoded in and out, still finite and in range of the inputs' encoding.
    ImagePtr legacy = run(n.get(), {dark, mid, bright}, false);
    for (size_t i = 0; i < legacy->pixelCount(); ++i) REQUIRE(std::isfinite(legacy->pixel(i)[0]));
}

TEST_CASE("HDR Merge passes a single image through") {
    auto n = NodeRegistry::instance().create("math.hdr_merge");
    ImagePtr s = scene(32, 24);
    std::vector<Value> in = {Value(s), Value()}, out(1);
    EvalContext ctx;
    ctx.colorManagement = ColorManagement::sceneLinear();
    n->evaluate(ctx, in, out);
    CHECK(toImage(out[0], 0, 0) == s);
}

TEST_CASE("Panorama Merge stitches overlapping photos back into the scene") {
    const int W = 600, H = 300, cw = 260;
    auto s = scene(W, H);
    // Shown at display brightness, so features read as in a normal photo.
    for (size_t i = 0; i < s->pixelCount(); ++i)
        for (int c = 0; c < 3; ++c) s->pixel(i)[c] = std::min(s->pixel(i)[c] * 0.3f, 1.0f);
    // Out of order, and the last one 15% brighter: gain compensation evens it out.
    std::vector<ImagePtr> photos = {crop(*s, 170, 0, cw, H, 1.0f, false), crop(*s, 0, 0, cw, H, 1.0f, false),
                                    crop(*s, 340, 0, cw, H, 1.15f, false)};
    auto n = NodeRegistry::instance().create("math.panorama_merge");
    REQUIRE(n);

    n->params[0] = 2;  // Perspective: plain translations stay exact
    ImagePtr r = run(n.get(), photos, true);
    CHECK(std::abs(r->w - W) <= 3);
    CHECK(std::abs(r->h - H) <= 3);
    // The middle photo is the reference, so the output lines up with the scene from x = 0.
    double e = 0;
    int cnt = 0;
    for (int y = 10; y < std::min(r->h, H) - 10; ++y)
        for (int x = 10; x < std::min(r->w, W) - 10; ++x) {
            const float a = r->pixel(size_t(y) * size_t(r->w) + size_t(x))[0];
            const float b = s->pixel(size_t(y) * size_t(W) + size_t(x))[0];
            e += std::abs(a - b), ++cnt;
        }
    CHECK(e / cnt < 0.03);

    // Spherical with Auto Crop: wide, and no empty corners left.
    n->params[0] = 0;
    n->params[1] = true;
    r = run(n.get(), photos, true);
    CHECK(r->w > 2 * cw);
    for (size_t i = 0; i < r->pixelCount(); ++i) REQUIRE(r->pixel(i)[3] > 0.99f);

    // Photos that don't overlap: the first one, unchanged.
    ImagePtr a = crop(*s, 0, 0, 100, 100, 1.0f, false);
    ImagePtr flat = std::make_shared<Image>(100, 100);
    ImagePtr only = run(n.get(), {a, flat}, true);
    CHECK(only == a);
}
