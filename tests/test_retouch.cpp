// Retouching: Sharpen (and export sharpening's shared core), Spot Removal.
#include <doctest/doctest.h>

#include <cmath>

#include <nlohmann/json.hpp>

#include "nodes/ImageOps.h"
#include "nodes/filter/SpotRemoval.h"

namespace {

// A soft vertical edge from `a` to `b` across a few pixels, with optional noise.
std::shared_ptr<Image> softEdge(float a, float b, float noise = 0.0f, int w = 64, int h = 16) {
    auto img = std::make_shared<Image>(w, h);
    uint32_t seed = 1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float t = std::clamp((x - (w / 2 - 2)) / 4.0f, 0.0f, 1.0f);
            seed = seed * 1664525u + 1013904223u;
            const float n = noise * (float(seed >> 8) / float(1 << 24) - 0.5f);
            float* p = img->pixel(size_t(y) * w + x);
            p[0] = p[1] = p[2] = a + (b - a) * t + n;
            p[3] = 1.0f;
        }
    return img;
}

float at(const Image& img, int x, int y = 8) { return img.pixel(size_t(y) * img.w + x)[0]; }

}  // namespace

TEST_CASE("Sharpen steepens edges, and Detail 0 adds no halos") {
    const auto src = softEdge(0.2f, 0.8f);
    imageops::SharpenSettings s;
    s.amount = 100, s.radius = 1.5f, s.detail = 100;
    Image full = *src;
    imageops::sharpenImage(full, s, false);
    // Steeper across the edge, with overshoot either side at full Detail.
    CHECK(at(full, 33) - at(full, 30) > at(*src, 33) - at(*src, 30));
    float lo = 1, hi = 0;
    for (int x = 0; x < src->w; ++x) lo = std::min(lo, at(full, x)), hi = std::max(hi, at(full, x));
    CHECK(lo < 0.19f);
    CHECK(hi > 0.81f);

    s.detail = 0;
    Image held = *src;
    imageops::sharpenImage(held, s, false);
    CHECK(at(held, 33) - at(held, 30) > at(*src, 33) - at(*src, 30));
    for (int x = 0; x < src->w; ++x) {
        CHECK(at(held, x) >= 0.2f - 1e-5f);
        CHECK(at(held, x) <= 0.8f + 1e-5f);
    }
}

TEST_CASE("Sharpen's Masking leaves flat noisy areas alone") {
    const auto src = softEdge(0.3f, 0.7f, 0.02f);
    imageops::SharpenSettings s;
    s.amount = 100, s.detail = 100, s.masking = 60;
    Image img = *src;
    imageops::sharpenImage(img, s, false);
    double flatChange = 0;
    for (int x = 2; x < 20; ++x) flatChange += std::fabs(at(img, x) - at(*src, x));
    CHECK(flatChange / 18 < 0.002);
    CHECK(std::fabs(at(img, 30) - at(*src, 30)) > 0.01f);  // the edge still sharpens
}

TEST_CASE("Sharpen keeps colour: neutral stays neutral and alpha is untouched") {
    auto src = softEdge(0.1f, 0.6f);
    for (size_t i = 0; i < src->px.size(); i += 4) src->px[i + 3] = 0.5f;
    Image img = *src;
    imageops::sharpenImage(img, {}, true);
    for (size_t i = 0; i < img.px.size(); i += 4) {
        CHECK(img.px[i] == doctest::Approx(img.px[i + 1]).epsilon(1e-5));
        CHECK(img.px[i + 3] == 0.5f);
    }
}

// ---------------------------------------------------------------- Spot Removal

namespace {

// A horizontal brightness ramp (0.2 .. 0.8) with fine vertical stripes, and a dark blemish.
std::shared_ptr<Image> blemished(int w = 128, int h = 64, bool blemish = true) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float v = 0.2f + 0.6f * x / (w - 1) + ((x / 2) % 2 ? 0.02f : -0.02f);
            if (blemish && std::hypot(x + 0.5f - 64, y + 0.5f - 32) < 5) v = 0.05f;
            float* p = img->pixel(size_t(y) * w + x);
            p[0] = p[1] = p[2] = v;
            p[3] = 1.0f;
        }
    return img;
}

float meanAbsDiff(const Image& a, const Image& b, float cx, float cy, float r) {
    double sum = 0;
    int n = 0;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x)
            if (std::hypot(x + 0.5f - cx, y + 0.5f - cy) < r) {
                sum += std::fabs(a.pixel(size_t(y) * a.w + x)[0] - b.pixel(size_t(y) * b.w + x)[0]);
                ++n;
            }
    return float(sum / std::max(n, 1));
}

}  // namespace

TEST_CASE("Spot Removal: Clone copies the source, Heal matches the surroundings") {
    const auto src = blemished(), clean = blemished(128, 64, false);
    Spot s;
    s.x = 64.0f / 128, s.y = 32.0f / 64;
    s.sx = 32.0f / 128, s.sy = 32.0f / 64;  // a darker part of the ramp, same stripes
    s.radius = 10.0f / 128, s.feather = 0.0f;

    // Clone: the inside is the source exactly (same stripe phase: an offset of 32 pixels).
    s.heal = false;
    Image cloned = *src;
    removeSpots(cloned, {s}, false);
    for (int x = 58; x <= 70; ++x) CHECK(cloned.pixel(32 * 128 + x)[0] == doctest::Approx(src->pixel(32 * 128 + x - 32)[0]));
    // ...so it is too dark for where it sits.
    const float cloneErr = meanAbsDiff(cloned, *clean, 64, 32, 9);
    CHECK(cloneErr > 0.1f);

    // Heal: the blemish is gone and the patch takes the ramp's brightness there, in both spaces.
    for (const bool linear : {false, true}) {
        CAPTURE(linear);
        s.heal = true;
        Image healed = *src;
        removeSpots(healed, {s}, linear);
        CHECK(meanAbsDiff(healed, *clean, 64, 32, 9) < 0.03f);
        // Nothing changes outside the circle.
        CHECK(healed.pixel(32 * 128 + 80)[0] == src->pixel(32 * 128 + 80)[0]);
        CHECK(healed.pixel(5 * 128 + 64)[0] == src->pixel(5 * 128 + 64)[0]);
    }
}

TEST_CASE("Spot Removal: opacity, feathering, edges and odd values") {
    const auto src = blemished();
    Spot s;
    s.x = 0.5f, s.y = 0.5f, s.sx = 0.25f, s.sy = 0.5f, s.radius = 0.08f;
    s.opacity = 0.0f;
    Image img = *src;
    removeSpots(img, {s}, false);
    CHECK(img.px == src->px);

    // Feathered: the centre is replaced, the rim only partly.
    s.opacity = 1.0f, s.feather = 1.0f, s.heal = false;
    img = *src;
    removeSpots(img, {s}, false);
    CHECK(img.pixel(32 * 128 + 64)[0] > 0.3f);

    // Spots at or past the edges, a source outside the image, NaN and zero sizes: no crash, finite.
    std::vector<Spot> odd(5, s);
    odd[0].x = 0.0f, odd[0].y = 1.0f;
    odd[1].sx = -0.5f, odd[1].sy = 1.7f, odd[1].heal = true;
    odd[2].radius = 0.0f;
    odd[3].radius = std::nanf("");
    odd[4].x = 3.0f;
    img = *src;
    removeSpots(img, odd, true);
    for (float v : img.px) CHECK(std::isfinite(v));
}

TEST_CASE("Spot Removal node keeps its spots in the project and its cache key") {
    SpotRemovalNode n;
    n.initParams();
    const std::string empty = n.signatureExtra();
    n.params[0] = 1;  // Clone
    n.params[1] = 0.05f;
    const int i = n.addSpot(0.8f, 0.3f, 1.5f);
    CHECK(i == 0);
    CHECK(n.active == 0);
    CHECK_FALSE(n.spots[0].heal);
    CHECK(n.spots[0].radius == doctest::Approx(0.05f));
    // The source starts beside it, toward the middle, and inside the image.
    CHECK(n.spots[0].sx < 0.8f);
    CHECK(n.spots[0].sx - 0.05f >= 0.0f);
    CHECK(n.signatureExtra() != empty);

    nlohmann::json j;
    n.saveExtra(j);
    SpotRemovalNode back;
    back.initParams();
    back.loadExtra(j);
    REQUIRE(back.spots.size() == 1);
    CHECK(back.spots[0].x == doctest::Approx(0.8f));
    CHECK_FALSE(back.spots[0].heal);
    CHECK(back.signatureExtra() == n.signatureExtra());

    // The params edit the selected spot.
    n.params[2] = 0.1f;
    CHECK(n.storeActive());
    CHECK(n.spots[0].feather == doctest::Approx(0.1f));
    CHECK_FALSE(n.storeActive());

    // Damaged spot data loads as defaults instead of failing.
    back.loadExtra(nlohmann::json::parse(R"({"spots":[{"x":"a","radius":1e9},{},7,null]})"));
    REQUIRE(back.spots.size() == 2);
    CHECK(back.spots[0].radius <= 0.3f);
}

TEST_CASE("Spot Removal: the automatic source matches the spot's surroundings") {
    // Left: horizontal stripes 6 px apart; right: flat. A blemish on the stripes, and another
    // just beside it (where the default source would land).
    const int w = 200, h = 120;
    Image img(w, h);
    const auto blemish = [](int x, int y, float cx, float cy) { return std::hypot(x - cx, y - cy) < 4.0f; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float v = x < 100 ? ((y / 3) % 2 ? 0.6f : 0.4f) : 0.5f;
            if (blemish(x, y, 50, 60) || blemish(x, y, 65, 60)) v = 0.05f;
            float* p = img.pixel(size_t(y) * w + x);
            p[0] = p[1] = p[2] = v, p[3] = 1;
        }
    std::vector<Spot> spots(1);
    Spot& s = spots[0];
    s.x = 50.0f / w, s.y = 60.0f / h, s.radius = 0.03f;  // 6 px
    REQUIRE(findSpotSource(img, spots, 0, false));
    const float sx = s.sx * w, sy = s.sy * h;
    CAPTURE(sx);
    CAPTURE(sy);
    CHECK(sx + 1.2f * 6 < 100);                     // on the stripes
    CHECK(std::hypot(sx - 50, sy - 60) >= 2.2f * 6);  // clear of the spot
    CHECK(std::hypot(sx - 65, sy - 60) >= 6 + 4);    // and of the other blemish
    const float phase = std::fmod(std::fabs(sy - 60), 6.0f);
    CHECK((phase < 0.5f || phase > 5.5f));  // stripes in step

    // "/": somewhere else.
    const float oldX = sx, oldY = sy;
    REQUIRE(findSpotSource(img, spots, 0, true));
    CHECK(std::hypot(s.sx * w - oldX, s.sy * h - oldY) >= 2 * 6);

    // Another spot's target is never a source.
    spots.push_back(spots[0]);
    spots[1].x = spots[0].sx, spots[1].y = spots[0].sy;
    REQUIRE(findSpotSource(img, spots, 0, false));
    CHECK(std::hypot((spots[0].sx - spots[1].x) * w, (spots[0].sy - spots[1].y) * h) >= 12);
}

TEST_CASE("Spot Removal: Clone's automatic source matches colours too") {
    // A left-to-right ramp: Clone needs a source at the same brightness (straight above or below).
    const int w = 160, h = 160;
    Image img(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img.pixel(size_t(y) * w + x);
            p[0] = p[1] = p[2] = x / float(w), p[3] = 1;
        }
    std::vector<Spot> spots(1);
    spots[0].x = 0.5f, spots[0].y = 0.5f, spots[0].radius = 0.03f, spots[0].heal = false;
    REQUIRE(findSpotSource(img, spots, 0, false));
    CHECK(std::fabs(spots[0].sx - 0.5f) * w < 1.0f);
    // Too small an image for a source (the spot fills it): nothing moves.
    Image tiny(6, 6);
    spots[0].radius = 0.3f;
    spots[0].sx = 0.7f;
    CHECK_FALSE(findSpotSource(tiny, spots, 0, false));
    CHECK(spots[0].sx == 0.7f);
    CHECK_FALSE(findSpotSource(img, spots, 3, false));
}
