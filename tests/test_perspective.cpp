// Perspective (Lightroom's Transform panel): slider directions, Guided Upright and Constrain Crop.
#include <doctest/doctest.h>

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
