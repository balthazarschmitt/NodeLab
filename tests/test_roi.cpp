#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>
#include <thread>

#include "graph/Evaluator.h"
#include "graph/NodeRegistry.h"
#include "io/ImageCache.h"
#include "io/ImageIO.h"
#include "io/Paths.h"
#include "nodes/NodeUtil.h"

namespace {

// A source with detail at every scale, like a photo: it knows its full size (the working size) and
// renders only the part a region asks for, as Image Input does.
class RoiTestSource : public Node {
public:
    NODELAB_NODE({"test.roi_source", "ROI Test Source", "Input", {}, {{"Image", PinType::Image}}, {}, true})
    bool roiSourceSize(const EvalContext& ctx, int& w, int& h) const override {
        w = ctx.defaultW;
        h = ctx.defaultH;
        return true;
    }
    void evaluate(EvalContext& ctx, const std::vector<Value>&, std::vector<Value>& out) override {
        PixelRect r{0, 0, ctx.defaultW, ctx.defaultH};
        if (ctx.roi) r = ctx.roi->rect;
        auto img = std::make_shared<Image>(r.w, r.h);
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) {
                const int gx = x + r.x, gy = y + r.y;
                float* p = img->pixel(size_t(y) * r.w + x);
                const unsigned hsh = unsigned(gx * 73856093) ^ unsigned(gy * 19349663);
                p[0] = 0.5f + 0.4f * std::sin(gx * 0.31f) * std::cos(gy * 0.17f);
                p[1] = float(gy) / ctx.defaultH;
                p[2] = float(hsh % 1000) / 1000.0f;
                p[3] = 0.75f + 0.25f * std::sin(gx * 0.05f + gy * 0.08f);
            }
        out[0] = Value(ImagePtr(img));
    }
};

void registerSource() {
    static bool done = false;
    if (done) return;
    done = true;
    NodeRegistry::instance().add<RoiTestSource>();
}

EvalContext roiCtx() {
    EvalContext ctx;
    ctx.defaultW = 120;
    ctx.defaultH = 80;
    ctx.scale = 0.5f;
    ctx.proxy = false;
    ctx.colorManagement.linear = true;
    return ctx;
}

// Max difference between a region and the same part of the whole image (-1: sizes differ).
float regionError(const Image& whole, const Evaluator::RegionResult& r) {
    const int x0 = int(std::lround(r.u0 * whole.w)), y0 = int(std::lround(r.v0 * whole.h));
    if (x0 + r.image->w > whole.w || y0 + r.image->h > whole.h) return -1;
    float err = 0;
    for (int y = 0; y < r.image->h; ++y)
        for (int x = 0; x < r.image->w; ++x) {
            const float* a = r.image->pixel(size_t(y) * r.image->w + x);
            const float* b = whole.pixel(size_t(y + y0) * whole.w + x + x0);
            for (int k = 0; k < 4; ++k)
                if (std::isfinite(a[k]) || std::isfinite(b[k])) err = std::max(err, std::abs(a[k] - b[k]));
        }
    return err;
}

using Params = std::vector<std::pair<std::string, nlohmann::json>>;

void setParams(Node& n, const Params& params) {
    for (const auto& [name, value] : params) {
        const auto& descs = n.info().params;
        auto it = std::find_if(descs.begin(), descs.end(), [&](const ParamDesc& d) { return d.name == name; });
        REQUIRE_MESSAGE(it != descs.end(), name);
        n.params[size_t(it - descs.begin())] = value;
    }
}

// Evaluates node `id` of g whole and by two regions; true if any region was evaluated (they must
// match the whole image).
bool checkRegions(const Graph& g, int id) {
    bool any = false;
    const Node* n = g.find(id);
    for (int o = 0; o < int(n->info().outputs.size()); ++o) {
        CAPTURE(o);
        for (const std::array<float, 4>& box :
             {std::array<float, 4>{0.3f, 0.25f, 0.62f, 0.7f}, std::array<float, 4>{0.0f, 0.5f, 0.45f, 1.0f}}) {
            EvalContext ctx = roiCtx();
            Evaluator ev;
            Value whole = ev.evaluateOutput(g, id, o, ctx);
            int ww, wh;
            if (!whole.size(ww, wh)) continue;
            auto r = ev.evaluateRegion(g, id, o, ctx, box[0], box[1], box[2], box[3]);
            if (!r) continue;
            any = true;
            ImagePtr wi = toImage(whole, ww, wh);
            CHECK(regionError(*wi, *r) <= 2e-5f);
            CHECK(r->u0 <= box[0]);
            CHECK(r->v1 >= box[3]);
        }
    }
    return any;
}

// The source connected to every input of one node.
bool checkNode(const std::string& type, const Params& params = {}) {
    Graph g;
    Node* src = g.addNode("test.roi_source");
    Node* split = g.addNode("color.split_rgb");
    g.connect(src->id, 0, split->id, 0);
    Node* n = g.addNode(type);
    REQUIRE(n);
    setParams(*n, params);
    for (int i = 0; i < int(n->info().inputs.size()); ++i) {
        const PinType t = n->info().inputs[i].type;
        if (t == PinType::Image) g.connect(src->id, 0, n->id, i);
        else if (t == PinType::Channel) g.connect(split->id, 1 + i % 3, n->id, i);
    }
    return checkRegions(g, n->id);
}

}  // namespace

// The core promise of region evaluation: a zoomed-in region looks exactly like the same part of
// the whole image. Nodes that can't promise it must make the evaluator fall back (no result).
TEST_CASE("every node's region matches the same part of its whole image") {
    registerSource();
    std::set<std::string> regional;
    for (const auto& type : NodeRegistry::instance().types()) {
        const NodeInfo* inf = NodeRegistry::instance().find(type);
        if (inf->hidden || inf->outputs.empty()) continue;
        CAPTURE(type);
        if (checkNode(type)) regional.insert(type);
    }
    std::string list;
    for (const auto& t : regional) list += t + " ";
    MESSAGE(regional.size() << " node types evaluate by region: " << list);
    // Nodes that must not fall back to the preview, or zoomed-in viewing is pointless for photos.
    for (const char* t : {"color.exposure", "color.basic", "filter.blur", "xform.crop", "conv.expression",
                          "conv.image_expression", "conv.normalize", "color.curves", "math.mix", "tex.noise",
                          "matte.radial_gradient", "util.split"}) {
        CAPTURE(t);
        CHECK(regional.count(t));
    }
}

// Settings that depend on position or on the image size.
TEST_CASE("regions match with position- and size-dependent settings") {
    registerSource();
    const std::vector<std::pair<std::string, Params>> cases = {
        {"conv.expression", {{"Expression", "x / w + y * 0.01 + u - v * h / 100"}}},
        {"conv.image_expression", {{"R", "u * r"}, {"G", "v + x * 0.001"}, {"B", "sin(y / 3) * w / 100"}}},
        {"filter.blur", {{"Size X", 40.0}, {"Size Y", 7.0}}},
        {"filter.blur", {{"Relative", true}, {"Factor X", 5.0}, {"Factor Y", 3.0}, {"Aspect Correction", 1}}},
        {"filter.bilateral_blur", {{"Radius", 20.0}}},
        {"filter.dilate_erode", {{"Mode", 1}, {"Distance", -9.0}}},
        {"filter.dilate_erode", {{"Distance", 12.0}}},
        {"filter.kuwahara", {{"Size", 15.0}}},
        {"filter.filter", {{"Type", 0}}},
        {"xform.crop", {{"Left", 0.1}, {"Right", 0.8}, {"Top", 0.2}, {"Bottom", 0.9}}},
        {"xform.crop", {{"Left", 0.1}, {"Right", 0.8}, {"Angle", 7.5}}},
        {"xform.crop", {{"Angle", -12.0}, {"Resize Image", false}, {"Constrain to Image", false}}},
        {"xform.crop", {{"Aspect", 2}, {"Top", 0.1}}},
        {"xform.flip", {{"Axis", 2}}},
        {"color.basic", {{"Clarity", 60.0}, {"Texture", -40.0}, {"Highlights", -50.0}, {"Shadows", 40.0}}},
        {"color.basic", {{"Whites", 30.0}, {"Blacks", -20.0}, {"Contrast", 25.0}}},
        {"color.basic", {{"Dehaze", 50.0}, {"Clarity", -30.0}}},
        {"color.basic", {{"Dehaze", -40.0}, {"Texture", 70.0}}},
        {"conv.normalize", {{"Low %", 5.0}, {"High %", 95.0}}},
        {"util.split", {{"Position", 0.4}, {"Orientation", 1}}},
        {"matte.box_mask", {{"X", 0.4}, {"Rotation", 30.0}, {"Feather", 0.3}}},
        {"matte.linear_gradient", {{"Start X", 0.1}, {"End Y", 0.9}}},
        {"tex.gradient", {{"Type", 6}, {"Angle", 30.0}}},
        {"tex.white_noise", {{"Grain Size", 6.0}}},
        {"tex.wave", {{"Type", 1}, {"Distortion", 3.0}}},
    };
    for (const auto& [type, params] : cases) {
        CAPTURE(type);
        CAPTURE(nlohmann::json(params).dump());
        CHECK(checkNode(type, params));
    }
}

// A photo-style chain: regions propagate through padding, mapping and generators together.
TEST_CASE("regions match through a chain of nodes") {
    registerSource();
    Graph g;
    Node* src = g.addNode("test.roi_source");
    Node* crop = g.addNode("xform.crop");
    setParams(*crop, {{"Left", 0.05}, {"Right", 0.9}, {"Angle", 4.0}});
    Node* basic = g.addNode("color.basic");
    setParams(*basic, {{"Clarity", 50.0}, {"Exposure", 0.5}});
    Node* blur = g.addNode("filter.blur");
    setParams(*blur, {{"Size X", 25.0}, {"Size Y", 25.0}});
    Node* mask = g.addNode("matte.radial_gradient");
    Node* mix = g.addNode("math.mix");
    // Generators (the mask) take the working size, so they go before the size-changing Crop.
    g.connect(src->id, 0, basic->id, 0);
    g.connect(basic->id, 0, blur->id, 0);
    g.connect(mask->id, 0, mix->id, 0);
    g.connect(basic->id, 0, mix->id, 1);
    g.connect(blur->id, 0, mix->id, 2);
    g.connect(mix->id, 0, crop->id, 0);
    Node* out = g.addNode("io.output");
    g.connect(crop->id, 0, out->id, 0);
    CHECK(checkRegions(g, crop->id));
    // The Output node shows its input.
    EvalContext ctx = roiCtx();
    Evaluator ev;
    ev.evaluateDisplay(g, out->id, ctx);
    CHECK(ev.evaluateRegion(g, out->id, 0, ctx, 0.2f, 0.2f, 0.5f, 0.5f));
}

TEST_CASE("region evaluation reuses the preview's statistics and cache levels stay separate") {
    registerSource();
    Graph g;
    Node* src = g.addNode("test.roi_source");
    Node* blur = g.addNode("filter.blur");
    g.connect(src->id, 0, blur->id, 0);
    EvalContext ctx = roiCtx();
    Evaluator ev;
    ev.evaluateOutput(g, blur->id, 0, ctx);
    const size_t previewBytes = ev.bytes(Evaluator::Preview);
    CHECK(previewBytes > 0);
    auto r = ev.evaluateRegion(g, blur->id, 0, ctx, 0.4f, 0.4f, 0.6f, 0.6f);
    REQUIRE(r);
    CHECK(ev.bytes(Evaluator::Region) < previewBytes);
    CHECK(ev.bytes(Evaluator::Preview) == previewBytes);  // the preview stays cached
    // The same region again is cached.
    const int before = ev.recomputeCount;
    ev.evaluateRegion(g, blur->id, 0, ctx, 0.4f, 0.4f, 0.6f, 0.6f);
    CHECK(ev.recomputeCount == before);
    // Without a preview there's nothing to plan from.
    Evaluator fresh;
    CHECK_FALSE(fresh.evaluateRegion(g, blur->id, 0, ctx, 0.4f, 0.4f, 0.6f, 0.6f));
}

TEST_CASE("releasing intermediates keeps the result and drops the rest") {
    registerSource();
    Graph g;
    Node* src = g.addNode("test.roi_source");
    Node* a = g.addNode("color.exposure");
    Node* b = g.addNode("filter.blur");
    Node* m = g.addNode("math.mix");
    g.connect(src->id, 0, a->id, 0);
    g.connect(a->id, 0, b->id, 0);
    g.connect(a->id, 0, m->id, 1);
    g.connect(b->id, 0, m->id, 2);
    EvalContext ctx = roiCtx();
    Evaluator keep, release;
    release.releaseIntermediates = true;
    Value v1 = keep.evaluateOutput(g, m->id, 0, ctx);
    Value v2 = release.evaluateOutput(g, m->id, 0, ctx);
    ImagePtr i1 = toImage(v1, 0, 0), i2 = toImage(v2, 0, 0);
    REQUIRE(i1);
    REQUIRE(i2);
    CHECK(i1->px == i2->px);
    // Only the result is left.
    CHECK(release.bytes(Evaluator::Preview) == i2->px.size() * sizeof(float));
}

TEST_CASE("crop and resample helpers") {
    Image img(4, 3);
    for (size_t i = 0; i < img.pixelCount(); ++i) img.pixel(i)[0] = float(i);
    ImagePtr c = cropImage(img, 1, 1, 2, 2);
    CHECK(c->w == 2);
    CHECK(c->pixel(0)[0] == 5.0f);
    CHECK(c->pixel(3)[0] == 10.0f);
    Value r = resampleValue(Value(ImagePtr(std::make_shared<Image>(img))), 8, 6);
    int w, h;
    REQUIRE(r.size(w, h));
    CHECK(w == 8);
    CHECK(toImage(r, 0, 0)->pixel(size_t(5) * 8 + 7)[0] == 11.0f);
    CHECK(cropValue(Value(0.5f), 0, 0, 1, 1).size(w, h) == false);
}

TEST_CASE("image cache levels and proxy edges") {
    int w, h;
    ImageCache::levelSize(6000, 4000, 1.0f, w, h);
    CHECK(w == 6000);
    ImageCache::levelSize(6000, 4000, 0.5f, w, h);
    CHECK(w == 3000);
    CHECK(h == 2000);
    ImageCache::levelSize(6000, 4000, 0.25f, w, h);
    CHECK(w == 1500);
    CHECK(h == 1000);
}

// The viewer's background evaluation: proxy size, drafts at half of it, and sharp details of a
// zoomed-in view (Phase F).
TEST_CASE("background evaluation drafts, sizes proxies and adds details") {
    Image src(400, 300);
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            float* p = src.pixel(size_t(y) * src.w + x);
            p[0] = float((x ^ y) & 1);
            p[1] = x / 399.0f;
            p[2] = y / 299.0f;
            p[3] = 1.0f;
        }
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "nodelab_async_test.png";
    std::string err;
    REQUIRE(saveImage(pathToU8(path), src, err));

    Graph g;
    Node* in = g.addNode("io.image_input");
    in->params[0] = pathToU8(path);
    Node* out = g.addNode("io.output");
    g.connect(in->id, 0, out->id, 0);

    ImageCache cache;
    AsyncEvaluator ev(cache);
    auto wait = [&](bool tiles) {
        for (int i = 0; i < 2000; ++i) {
            if (auto r = ev.poll(); r && (!tiles || r->tilesDone)) return *r;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        FAIL("no result");
        return AsyncEvaluator::Result{};
    };
    AsyncEvaluator::Options opt;
    opt.proxyEdge = 200;
    ev.submit(g.toJson(), {{out->id}}, {0}, false, opt);
    AsyncEvaluator::Result r = wait(false);
    REQUIRE(r.images[0]);
    CHECK(r.images[0]->w == 200);
    CHECK(!r.draft);

    opt.draft = true;
    ev.submit(g.toJson(), {{out->id}}, {0}, false, opt);
    r = wait(false);
    REQUIRE(r.images[0]);
    CHECK(r.images[0]->w == 100);
    CHECK(r.draft);

    // A view showing the image 800 pixels wide, zoomed into its right half: full resolution there.
    opt.draft = false;
    opt.details = {AsyncEvaluator::Detail{7, out->id, 0, 0.5f, 0.25f, 1.0f, 0.75f, 800.0f}};
    ev.submit(g.toJson(), {{out->id}}, {0}, false, opt);
    r = wait(true);
    REQUIRE(r.tiles.size() == 1);
    CHECK(r.tiles[0].tag == 7);
    REQUIRE(r.tiles[0].image);
    const Image& t = *r.tiles[0].image;
    const int x0 = int(std::lround(r.tiles[0].u0 * 400)), y0 = int(std::lround(r.tiles[0].v0 * 300));
    CHECK(x0 <= 200);
    CHECK(t.w == int(std::lround((r.tiles[0].u1 - r.tiles[0].u0) * 400)));
    float maxErr = 0;
    for (int y = 0; y < t.h; ++y)
        for (int x = 0; x < t.w; ++x)
            for (int k = 0; k < 4; ++k)
                maxErr = std::max(maxErr, std::fabs(t.pixel(size_t(y) * t.w + x)[k] -
                                                    src.pixel(size_t(y + y0) * src.w + x + x0)[k]));
    CHECK(maxErr <= 0.5f / 255 + 1e-5f);  // the PNG holds 8 bits

    // Fitted (not zoomed in): the preview is as sharp as the screen, so no detail image.
    opt.details[0].screenW = 200.0f;
    ev.submit(g.toJson(), {{out->id}}, {0}, false, opt);
    r = wait(true);
    REQUIRE(r.tiles.size() == 1);
    CHECK(!r.tiles[0].image);
    std::filesystem::remove(path);
}
