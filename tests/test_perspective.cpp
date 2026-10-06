// Perspective (Lightroom's Transform panel): slider directions, Guided Upright and Constrain Crop.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "graph/NodeRegistry.h"
#include "nodes/transform/TransformNodes.h"

using namespace perspective;

namespace {

std::unique_ptr<PerspectiveNode> make() {
    auto n = std::make_unique<PerspectiveNode>();
    n->initParams();
    return n;
}

// Where an input point lands in the output.
void forward(const PerspectiveNode& n, int w, int h, double u, double v, double& ou, double& ov) {
    REQUIRE(apply(inverse(n.matrix(w, h)), u, v, ou, ov));
}

void back(const PerspectiveNode& n, int w, int h, double u, double v, double& iu, double& iv) {
    REQUIRE(apply(n.matrix(w, h), u, v, iu, iv));
}

}  // namespace

TEST_CASE("Perspective: neutral settings pass the image through") {
    auto n = make();
    auto img = std::make_shared<Image>(8, 6);
    std::vector<Value> out(1);
    EvalContext ctx;
    n->evaluate(ctx, {Value(img)}, out);
    CHECK(toImage(out[0], 0, 0) == img);
    // Guides do nothing while Upright is Off.
    n->guides.push_back({0.2f, 0.1f, 0.3f, 0.9f});
    n->params[Upright] = int(UprightOff);
    n->evaluate(ctx, {Value(img)}, out);
    CHECK(toImage(out[0], 0, 0) == img);
}

TEST_CASE("Perspective: slider directions") {
    const int w = 300, h = 200;
    double a, b, c, d, tmp;
    // Vertical below zero widens the top: the top row shows less of the input than the bottom.
    auto n = make();
    n->params[Vertical] = -50.0f;
    back(*n, w, h, 0, 0, a, tmp);
    back(*n, w, h, 1, 0, b, tmp);
    back(*n, w, h, 0, 1, c, tmp);
    back(*n, w, h, 1, 1, d, tmp);
    CHECK(b - a < d - c);
    // Horizontal above zero widens the right side.
    n = make();
    n->params[Horizontal] = 50.0f;
    back(*n, w, h, 0, 0, tmp, a);
    back(*n, w, h, 0, 1, tmp, b);
    back(*n, w, h, 1, 0, tmp, c);
    back(*n, w, h, 1, 1, tmp, d);
    CHECK(d - c < b - a);
    // Rotate turns clockwise: a point right of the centre moves down.
    n = make();
    n->params[Rotate] = 5.0f;
    forward(*n, w, h, 0.9, 0.5, tmp, a);
    CHECK(a > 0.5);
    // Offsets: X right, Y up.
    n = make();
    n->params[OffsetX] = 20.0f;
    n->params[OffsetY] = 20.0f;
    forward(*n, w, h, 0.5, 0.5, a, b);
    CHECK(a == doctest::Approx(0.6));
    CHECK(b == doctest::Approx(0.4));
    // Scale 200 would double: 150 zooms in by half again.
    n = make();
    n->params[Scale] = 150.0f;
    forward(*n, w, h, 0.7, 0.5, a, b);
    CHECK(a == doctest::Approx(0.8));
}

TEST_CASE("Perspective: Guided Upright makes the guides vertical and horizontal") {
    const int w = 400, h = 300;
    // Lines that were upright before a tilt (A), drawn as guides on A's output, come out
    // upright from B.
    auto tilt = make();
    tilt->params[Vertical] = 40.0f;
    tilt->params[Rotate] = 4.0f;
    const auto through = [&](double u, double v, float& ou, float& ov) {
        double x, y;
        forward(*tilt, w, h, u, v, x, y);
        ou = float(x), ov = float(y);
    };
    Guide g[3];
    through(0.25, 0.2, g[0].x0, g[0].y0);
    through(0.25, 0.8, g[0].x1, g[0].y1);
    through(0.75, 0.2, g[1].x0, g[1].y0);
    through(0.75, 0.8, g[1].x1, g[1].y1);
    through(0.2, 0.6, g[2].x0, g[2].y0);
    through(0.8, 0.6, g[2].x1, g[2].y1);
    auto fix = make();
    fix->guides = {g[0], g[1], g[2]};
    REQUIRE(fix->paramI(Upright) == UprightGuided);
    CHECK(g[0].vertical(float(w) / h));
    CHECK_FALSE(g[2].vertical(float(w) / h));
    // Before: the verticals lean.
    CHECK(std::fabs(g[0].x1 - g[0].x0) > 0.01f);
    double x0, y0, x1, y1;
    for (int i = 0; i < 2; ++i) {
        forward(*fix, w, h, g[i].x0, g[i].y0, x0, y0);
        forward(*fix, w, h, g[i].x1, g[i].y1, x1, y1);
        CHECK(std::fabs(x1 - x0) < 1e-4);
    }
    forward(*fix, w, h, g[2].x0, g[2].y0, x0, y0);
    forward(*fix, w, h, g[2].x1, g[2].y1, x1, y1);
    CHECK(std::fabs(y1 - y0) < 1e-4);

    // One vertical guide is enough to stand it up, without turning anything else.
    fix->guides = {g[0]};
    forward(*fix, w, h, g[0].x0, g[0].y0, x0, y0);
    forward(*fix, w, h, g[0].x1, g[0].y1, x1, y1);
    CHECK(std::fabs(x1 - x0) < 1e-4);
    // A degenerate guide (a point) is ignored.
    fix->guides = {{0.5f, 0.5f, 0.5f, 0.5f}};
    double ang[3];
    fix->guidedAngles(w, h, ang);
    CHECK(ang[0] == 0.0);
}

TEST_CASE("Perspective: Constrain Crop leaves no empty corners") {
    auto n = make();
    n->params[Vertical] = -70.0f;
    n->params[Rotate] = -6.0f;
    n->params[Constrain] = true;
    auto img = std::make_shared<Image>(60, 40);
    for (size_t i = 0; i < img->px.size(); ++i) img->px[i] = 1.0f;
    std::vector<Value> out(1);
    EvalContext ctx;
    n->evaluate(ctx, {Value(img)}, out);
    ImagePtr o = toImage(out[0], 0, 0);
    REQUIRE(o);
    REQUIRE(o->w == 60);
    for (int x = 0; x < o->w; ++x) {
        CHECK(o->pixel(size_t(x))[3] > 0.99f);
        CHECK(o->pixel(size_t(o->h - 1) * o->w + x)[3] > 0.99f);
    }
    // Without it the narrowed bottom leaves transparent corners.
    n->params[Constrain] = false;
    n->evaluate(ctx, {Value(img)}, out);
    o = toImage(out[0], 0, 0);
    CHECK(o->pixel(size_t(o->h - 1) * o->w)[3] < 0.01f);
}

TEST_CASE("Perspective: guides are saved, and damaged ones dropped") {
    auto n = make();
    n->guides = {{0.1f, 0.2f, 0.15f, 0.9f}, {0.8f, 0.1f, 0.85f, 0.95f}};
    nlohmann::json j;
    n->saveExtra(j);
    auto m = make();
    m->loadExtra(j);
    REQUIRE(m->guides.size() == 2);
    CHECK(m->guides[1].x1 == 0.85f);
    CHECK(m->signatureExtra() == n->signatureExtra());
    m->loadExtra({{"guides", {{0.1, 0.2, 0.3}, "x", {0.1, "a", 0.2, 0.3}, {5, -1, 0.5, 0.5}, {0, 0, 1, 1}, {0, 0, 1, 1},
                              {0, 0, 1, 1}, {0, 0, 1, 1}}}});
    REQUIRE(m->guides.size() == kMaxGuides);
    CHECK(m->guides[0].x0 == 1.0f);  // clamped into the image
    CHECK(m->guides[0].y0 == 0.0f);
    m->loadExtra({{"guides", 3}});
    CHECK(m->guides.empty());
}

namespace {

// A grid of dark lines on white, turned by `deg` degrees; with `converge`, the verticals lean in
// towards the top as a building photographed from below.
ImagePtr gridImage(int w, int h, double deg, double converge = 0.0) {
    auto img = std::make_shared<Image>(w, h);
    const double a = deg * 3.14159265358979 / 180.0, cx = w * 0.5, cy = h * 0.5;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            // 4x4 samples per pixel: antialiased, as a photo's edges are.
            float sum = 0;
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) {
                    const double sx = x + (i + 0.5) / 4 - cx, sy = y + (j + 0.5) / 4 - cy;
                    double u = std::cos(a) * sx + std::sin(a) * sy, v = -std::sin(a) * sx + std::cos(a) * sy;
                    u /= 1.0 - converge * (0.5 - (v + cy) / h);  // narrower towards the top
                    const bool line = std::fabs(std::remainder(u, 50.0)) < 2.0 || std::fabs(std::remainder(v, 50.0)) < 2.0;
                    sum += line ? 0.05f : 0.9f;
                }
            float* p = img->pixel(size_t(y) * w + x);
            p[0] = p[1] = p[2] = sum / 16;
            p[3] = 1.0f;
        }
    return img;
}

// How far the near-vertical (or all near-upright) lines found lean, in degrees: the 75th
// percentile, as a few short lines cut across the grid's crossings.
double worstTilt(const Image& img, bool verticalOnly = false) {
    std::vector<double> tilts;
    for (const Segment& s : detectLines(img, false)) {
        const double dx = (s.x1 - s.x0) * img.w, dy = (s.y1 - s.y0) * img.h;
        const double a = std::atan2(std::fabs(dy), std::fabs(dx)) * 180.0 / 3.14159265358979;
        if (verticalOnly && a < 45.0) continue;
        if (std::min(a, 90.0 - a) < 15.0) tilts.push_back(std::min(a, 90.0 - a));
    }
    if (tilts.empty()) return 0;
    std::sort(tilts.begin(), tilts.end());
    return tilts[tilts.size() * 3 / 4];
}

}  // namespace

TEST_CASE("Perspective: line detection finds a turned grid") {
    const ImagePtr img = gridImage(400, 300, 4.0);
    const std::vector<Segment> segs = detectLines(*img, false);
    REQUIRE(segs.size() >= 6);
    // Nearly all of them follow the grid (a few short ones may cut across a crossing).
    int along = 0;
    for (const Segment& s : segs) {
        const double dx = (s.x1 - s.x0) * 400, dy = (s.y1 - s.y0) * 300;
        double a = std::atan2(std::fabs(dy), std::fabs(dx)) * 180.0 / 3.14159265358979;
        a = std::min(a, 90.0 - a);
        along += std::fabs(a - 4.0) < 0.5;
    }
    CHECK(along >= int(segs.size()) * 8 / 10);
    // A flat image or a tiny one has none.
    CHECK(detectLines(Image(400, 300), false).empty());
    CHECK(detectLines(Image(3, 3), false).empty());
}

TEST_CASE("Perspective: Upright Level, Vertical, Full and Auto straighten found lines") {
    EvalContext ctx;
    std::vector<Value> out(1);
    for (int mode : {int(UprightLevel), int(UprightAuto), int(UprightFull)}) {
        CAPTURE(mode);
        auto n = make();
        n->params[Upright] = mode;
        n->params[Constrain] = true;  // or the turned frame's own edges count as lines
        const ImagePtr img = gridImage(400, 300, 3.0);
        n->evaluate(ctx, {Value(img)}, out);
        const ImagePtr res = toImage(out[0], 0, 0);
        REQUIRE(res);
        CHECK(res != img);
        CHECK(worstTilt(*res) < 0.8);
        double ang[3];
        n->detectedAngles(ang);
        CHECK(std::fabs(ang[2]) * 180.0 / 3.14159265358979 == doctest::Approx(3.0).epsilon(0.15));
    }
    // Converging verticals: Vertical stands them up; Level only levels the horizon.
    const ImagePtr lean = gridImage(400, 300, 0.0, 0.25);
    CHECK(worstTilt(*lean, true) > 2.0);
    auto v = make();
    v->params[Upright] = int(UprightVertical);
    v->params[Constrain] = true;
    v->evaluate(ctx, {Value(lean)}, out);
    CHECK(worstTilt(*toImage(out[0], 0, 0), true) < 1.0);
    auto l = make();
    l->params[Upright] = int(UprightLevel);
    l->evaluate(ctx, {Value(lean)}, out);
    double ang[3];
    l->detectedAngles(ang);
    CHECK(std::fabs(ang[0]) < 1e-9);
    CHECK(std::fabs(ang[2]) < 0.01);
    // A region reuses the preview's rotation.
    std::vector<float> stats;
    EvalContext pre;
    pre.statsOut = &stats;
    v->evaluate(pre, {Value(lean)}, out);
    REQUIRE(stats.size() == 3);
    RoiWindow roi{};
    EvalContext reg;
    reg.roi = &roi;
    reg.previewStats = &stats;
    std::vector<Value> out2(1);
    v->evaluate(reg, {Value(ImagePtr(std::make_shared<Image>(400, 300)))}, out2);  // blank: nothing to find
    v->detectedAngles(ang);
    CHECK(float(ang[0]) == stats[0]);
}

TEST_CASE("Pan and Zoom: moves and zooms the picture, transparent outside") {
    auto n = NodeRegistry::instance().create(panzoom::kType);
    REQUIRE(n);
    auto img = std::make_shared<Image>(8, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x) {
            float* p = img->pixel(size_t(y) * 8 + x);
            p[0] = float(x), p[1] = float(y), p[2] = 0, p[3] = 1;
        }
    EvalContext ctx;
    std::vector<Value> out(1);
    auto run = [&] {
        n->evaluate(ctx, {Value(img)}, out);
        ImagePtr r = toImage(out[0], 0, 0);
        REQUIRE(r);
        REQUIRE(r->w == 8);
        REQUIRE(r->h == 4);
        return r;
    };
    // Neutral passes every pixel through.
    ImagePtr r = run();
    CHECK(r->pixel(5)[0] == doctest::Approx(5));
    CHECK(r->pixel(5)[3] == doctest::Approx(1));
    // X = 0.25 moves it a quarter of the width (2 px) right; the left strip is empty.
    n->params[panzoom::X] = 0.25f;
    n->params[panzoom::Interpolation] = 1;
    r = run();
    CHECK(r->pixel(0)[3] == 0);
    CHECK(r->pixel(1)[3] == 0);
    CHECK(r->pixel(2)[0] == 0);
    CHECK(r->pixel(7)[0] == 5);
    CHECK(r->pixel(7)[3] == 1);
    // Zoom 2 about the centre: output column 4 shows input column 4, column 0 shows column 2.
    n->params[panzoom::X] = 0.0f;
    n->params[panzoom::Zoom] = 2.0f;
    r = run();
    CHECK(r->pixel(0)[0] == 2);
    CHECK(r->pixel(4)[0] == 4);
    // Zoomed out, the border is transparent and the middle keeps its colour.
    n->params[panzoom::Zoom] = 0.5f;
    r = run();
    CHECK(r->pixel(0)[3] == 0);
    CHECK(r->pixel(size_t(1) * 8 + 4)[3] == 1);
}
