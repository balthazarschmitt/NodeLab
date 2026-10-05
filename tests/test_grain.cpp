// Grain (Lightroom's Effects > Grain): its strength, where it shows, and that the preview matches
// the export seen at the preview's size.
#include <doctest/doctest.h>

#include <cmath>

#include "graph/Graph.h"

namespace {

ImagePtr flat(int w, int h, float v) {
    auto img = std::make_shared<Image>(w, h);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        float* p = img->pixel(i);
        p[0] = p[1] = p[2] = v, p[3] = 1.0f;
    }
    return img;
}

ImagePtr grain(Node* n, const ImagePtr& img, float scale = 1.0f, bool linear = false) {
    std::vector<Value> in(n->info().inputs.size()), out(1);
    in[0] = Value(img);
    EvalContext ctx;
    ctx.defaultW = img->w, ctx.defaultH = img->h;
    ctx.scale = scale;
    ctx.colorManagement.linear = linear;
    n->evaluate(ctx, in, out);
    ImagePtr r = std::get<ImagePtr>(out[0].v);
    REQUIRE(r);
    return r;
}

struct Stats {
    double mean, sd;
};
Stats stats(const Image& img) {
    double s = 0, s2 = 0;
    const size_t n = size_t(img.w) * img.h;
    for (size_t i = 0; i < n; ++i) s += img.pixel(i)[0], s2 += double(img.pixel(i)[0]) * img.pixel(i)[0];
    const double m = s / n;
    return {m, std::sqrt(std::max(0.0, s2 / n - m * m))};
}

}  // namespace

TEST_CASE("Grain is about 8% at Amount 100 in the mid-tones, whatever its Roughness") {
    Graph g;
    Node* n = g.addNode("filter.grain");
    REQUIRE(n);
    n->params[0] = 100.0f;
    const ImagePtr grey = flat(256, 256, 0.5f);
    for (float rough : {0.0f, 50.0f, 100.0f})
        for (float size : {0.0f, 25.0f, 100.0f}) {
            CAPTURE(rough);
            CAPTURE(size);
            n->params[1] = size;
            n->params[2] = rough;
            const Stats st = stats(*grain(n, size < 100 ? grey : flat(512, 512, 0.5f)));
            CHECK(std::fabs(st.mean - 0.5) < 0.002);
            CHECK(st.sd > 0.08 * 0.85);
            CHECK(st.sd < 0.08 * 1.15);
        }
    // Black and white stay clean; Amount 0 changes nothing.
    CHECK(stats(*grain(n, flat(64, 64, 0.0f))).sd == 0.0);
    CHECK(stats(*grain(n, flat(64, 64, 1.0f))).sd == 0.0);
    n->params[0] = 0.0f;
    CHECK(stats(*grain(n, grey)).sd == 0.0);
}

TEST_CASE("Grain in a preview looks like the export scaled down to it") {
    Graph g;
    Node* n = g.addNode("filter.grain");
    REQUIRE(n);
    n->params[0] = 100.0f;
    // The full-resolution grain, box-averaged f x f, against a preview at 1/f.
    for (auto [size, f] : {std::pair{25.0f, 4}, std::pair{100.0f, 8}, std::pair{50.0f, 2}}) {
        CAPTURE(size);
        n->params[1] = size;
        const int pw = 128, fw = pw * f;
        const ImagePtr full = grain(n, flat(fw, fw, 0.5f), 1.0f, true);
        auto down = std::make_shared<Image>(pw, pw);
        for (int y = 0; y < pw; ++y)
            for (int x = 0; x < pw; ++x) {
                float s = 0;
                for (int j = 0; j < f; ++j)
                    for (int i = 0; i < f; ++i) s += full->pixel(size_t(y * f + j) * fw + x * f + i)[0];
                down->pixel(size_t(y) * pw + x)[0] = s / float(f * f);
            }
        const double previewSd = stats(*grain(n, flat(pw, pw, 0.5f), 1.0f / f, true)).sd;
        const double downSd = stats(*down).sd;
        CHECK(previewSd > downSd * 0.75);
        CHECK(previewSd < downSd * 1.33);
    }
}
