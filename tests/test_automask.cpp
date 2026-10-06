// AI masks (Select Subject, Select Sky): the model downloads' checksums, the nodes without their
// models, the Add Mask recipes, and (opt-in, NODELAB_ML_REAL=1) the installed models themselves.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <set>

#include "graph/Graph.h"
#include "graph/Recipes.h"
#include "io/Paths.h"
#include "ml/Models.h"
#include "ml/Onnx.h"
#include "nodes/matte/AutoMask.h"

namespace {

std::string sha(const std::string& s) {
    ml::Sha256 h;
    h.add(s.data(), s.size());
    return h.hex();
}

// Sky above, green hillside below (sRGB values, as in a legacy project).
ImagePtr landscape(int w, int h) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float* p = img->pixel(size_t(y) * w + x);
            const bool sky = y < h * 2 / 5 + int(10 * std::sin(x * 0.05));
            p[0] = sky ? 0.45f : 0.25f, p[1] = sky ? 0.65f : 0.4f, p[2] = sky ? 0.95f : 0.15f, p[3] = 1.0f;
        }
    return img;
}

ChannelPtr run(Node* n, std::vector<Value> in, int w, int h) {
    std::vector<Value> out(n->info().outputs.size());
    EvalContext ctx;
    ctx.defaultW = w, ctx.defaultH = h;
    in.resize(n->info().inputs.size());
    n->evaluate(ctx, in, out);
    ChannelPtr c = toChannel(out[0]);
    REQUIRE(c);
    return c;
}

double mean(const Channel& c, int y0, int y1) {
    double s = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < c.w; ++x) s += c.data[size_t(y) * c.w + x];
    return s / (double(y1 - y0) * c.w);
}

}  // namespace

TEST_CASE("SHA-256 matches the FIPS 180-4 test vectors") {
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // Fed in uneven pieces, across block boundaries.
    const std::string a(1000, 'a');
    ml::Sha256 h;
    for (size_t i = 0; i < a.size();) {
        const size_t n = std::min<size_t>(1 + i % 97, a.size() - i);
        h.add(a.data() + i, n);
        i += n;
    }
    CHECK(h.hex() == sha(a));
    CHECK(sha(a) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST_CASE("The model catalogue is pinned") {
    for (const char* id : {ml::kRuntime, "subject", "subject-light", "sky"}) {
        const ml::ModelSpec* m = ml::findModel(id);
        REQUIRE(m);
        CHECK(!m->files.empty());
        for (const ml::FileSpec& f : m->files) {
            CHECK(f.size > 0);
            CHECK(std::strlen(f.sha256) == 64);
            CHECK(std::string(f.url).rfind("https://", 0) == 0);
        }
    }
    CHECK(ml::findModel("nope") == nullptr);
}

TEST_CASE("Select Subject and Select Sky without their models give an empty mask") {
    if (std::getenv("NODELAB_ML_REAL")) return;  // the models are installed for that run
    REQUIRE(!ml::available("subject"));          // test_main points the models folder at an empty one
    CHECK(ml::downloadSize("sky") > 100'000'000);
    for (const char* type : {"matte.select_subject", "matte.select_sky", "matte.select_people", "matte.select_landscape",
                             "matte.select_objects"}) {
        Graph g;
        auto* n = dynamic_cast<AutoMaskNode*>(g.addNode(type));
        REQUIRE(n);
        const ImagePtr img = landscape(40, 30);
        ChannelPtr c = run(n, {Value(img)}, 40, 30);
        CHECK(c->w == 40);
        CHECK(c->h == 30);
        CHECK(mean(*c, 0, 30) == 0.0);
        CHECK(!n->lastError().empty());
        CHECK(n->signatureExtra().rfind("missing", 0) == 0);
        // Invert: everything; with a Mask input, that mask.
        n->params[AutoMaskNode::Invert] = true;
        CHECK(mean(*run(n, {Value(img)}, 40, 30), 0, 30) == doctest::Approx(1.0));
        CHECK(mean(*run(n, {Value(img), Value(0.25f)}, 40, 30), 0, 30) == doctest::Approx(0.25));
    }
}

TEST_CASE("Add Mask's Subject, Sky and Background read the image being adjusted") {
    Graph g;
    Node* in = g.addNode("io.image_input");
    Node* out = g.addNode("io.output", 400, 0);
    g.connect(in->id, 0, out->id, 0);
    const recipes::AddedMask s = recipes::addMask(g, recipes::MaskKind::Sky);
    REQUIRE(s.ok());
    CHECK(g.find(s.mask)->info().type == "matte.select_sky");
    CHECK(g.inputLink(s.mask, 0)->fromNode == in->id);
    CHECK_FALSE(g.find(s.mask)->paramB(AutoMaskNode::Invert));

    const recipes::AddedMask b = recipes::addMask(g, recipes::MaskKind::Background);
    REQUIRE(b.ok());
    CHECK(g.find(b.mask)->info().type == "matte.select_subject");
    CHECK(g.find(b.mask)->paramB(AutoMaskNode::Invert));
    CHECK(g.inputLink(b.mask, 0)->fromNode == s.adjust);

    Graph g2;
    Node* in2 = g2.addNode("io.image_input");
    Node* inv = g2.addNode("color.invert", 200, 0);
    Node* out2 = g2.addNode("io.output", 400, 0);
    g2.connect(in2->id, 0, inv->id, 0);
    g2.connect(inv->id, 0, out2->id, 0);
    const recipes::AddedMask m = recipes::maskNodes(g2, {inv->id}, recipes::MaskKind::Subject);
    REQUIRE(m.ok());
    CHECK(g2.find(m.mask)->info().type == "matte.select_subject");
    CHECK(g2.inputLink(m.mask, 0)->fromNode == in2->id);
}

// Opt-in: runs the installed models (`NodeLab.exe --install-model sky`, and subject).
TEST_CASE("The installed models run and find the sky") {
    if (!std::getenv("NODELAB_ML_REAL")) return;
    // The masks this run stores go into the real models folder's disk cache: remove them after,
    // so the user's own cached masks aren't pushed out and the next run starts from scratch.
    const std::filesystem::path cache = u8ToPath(ml::folder()) / "cache";
    std::set<std::filesystem::path> before;
    std::error_code ec;
    for (const auto& de : std::filesystem::directory_iterator(cache, ec)) before.insert(de.path());
    struct Cleanup {
        const std::filesystem::path& dir;
        const std::set<std::filesystem::path>& keep;
        ~Cleanup() {
            std::error_code ec;
            std::vector<std::filesystem::path> made;
            for (const auto& de : std::filesystem::directory_iterator(dir, ec))
                if (!keep.count(de.path())) made.push_back(de.path());
            for (const auto& p : made) std::filesystem::remove(p, ec);
        }
    } cleanup{cache, before};
    // Every model: its input size is the node's, and it makes a mask of a picture. (The accurate
    // subject model takes a minute, so it's only opened.)
    for (const char* type : {"matte.select_subject", "matte.select_subject_light", "matte.select_people",
                             "matte.select_landscape", "matte.select_objects", "matte.select_sky"}) {
        CAPTURE(std::string(type));
        const bool light = std::string(type) == "matte.select_subject_light";
        Graph g;
        auto* n = dynamic_cast<AutoMaskNode*>(g.addNode(light ? "matte.select_subject" : type));
        REQUIRE(n);
        if (light) n->params[2] = 1;  // Model: Light
        if (!ml::available(n->model().id)) continue;
        std::string err;
        const std::vector<int64_t> shape = ml::inputShape(n->model().id, err);
        CAPTURE(err);
        REQUIRE(shape.size() == 4);
        // SegFormer's exports take any size (-1); the node gives them the size they were trained at.
        CHECK((shape[2] == n->model().height || shape[2] == -1));
        CHECK((shape[3] == n->model().width || shape[3] == -1));
        if (std::string(type) == "matte.select_subject") continue;
        if (std::string(type) != "matte.select_sky") {
            const ChannelPtr c = run(n, {Value(landscape(300, 200))}, 300, 200);
            CHECK(n->lastError() == "");
            REQUIRE(c);
            CHECK(mean(*c, 0, 200) >= 0.0);
            continue;
        }
        const ImagePtr img = landscape(300, 200);
        ChannelPtr c = run(n, {Value(img)}, 300, 200);
        CHECK(n->lastError().empty());
        CHECK(mean(*c, 0, 50) > 0.7);
        CHECK(mean(*c, 120, 200) < 0.2);

        // Interactive previews don't wait: a new picture runs in the background, a brighter copy
        // of a known one (an exposure edit upstream) reuses its mask.
        std::vector<float> probs;
        int pw = 0, ph = 0;
        auto brighter = std::make_shared<Image>(*img);
        for (size_t i = 0; i < size_t(img->w) * img->h; ++i)
            for (int k = 0; k < 3; ++k) brighter->pixel(i)[k] = std::min(1.0f, img->pixel(i)[k] * 1.15f);
        CHECK(n->infer(*brighter, false, true, probs, pw, ph, err, nullptr) == AutoMaskNode::Inferred::Ready);
        auto other = std::make_shared<Image>(*landscape(300, 200));
        // A dark building in the sky, somewhere new each run: an earlier run's mask of the same
        // picture (left by a run that crashed) would be found in the disk cache.
        const int bx = 60 + int(std::chrono::steady_clock::now().time_since_epoch().count() % 120);
        for (int y = 0; y < 200; ++y)
            for (int x = bx; x < bx + 60; ++x)
                for (int k = 0; k < 3; ++k) other->pixel(size_t(y) * 300 + x)[k] = 0.1f;
        const int gen = AutoMaskNode::resultGeneration();
        CHECK(n->infer(*other, false, true, probs, pw, ph, err, nullptr) == AutoMaskNode::Inferred::Pending);
        AutoMaskNode::waitForRuns();
        CHECK(AutoMaskNode::resultGeneration() > gen);
        CHECK(n->infer(*other, false, true, probs, pw, ph, err, nullptr) == AutoMaskNode::Inferred::Ready);
    }
}

TEST_CASE("AI masks' Feather softens the outline and Edge grows or shrinks the mask") {
    // Without a model, the low-resolution mask comes in as a region's preview statistics: the
    // left half selected.
    const int w = 200, h = 100, pw = 20, ph = 10;
    std::vector<float> stats{float(pw), float(ph)};
    for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) stats.push_back(x < pw / 2 ? 1.0f : 0.0f);
    RoiWindow roi;
    roi.rect = {0, 0, w, h};
    roi.canvasW = w, roi.canvasH = h;
    const auto mid = [&](Node* n) {
        std::vector<Value> in(2), out(1);
        in[0] = Value(landscape(w, h));
        EvalContext ctx;
        ctx.defaultW = w, ctx.defaultH = h;
        ctx.roi = &roi;
        ctx.previewStats = &stats;
        n->evaluate(ctx, in, out);
        ChannelPtr c = toChannel(out[0]);
        REQUIRE(c);
        std::vector<float> row(w);
        for (int x = 0; x < w; ++x) row[size_t(x)] = c->data[size_t(h / 2) * w + x];
        return row;
    };
    const auto crossing = [](const std::vector<float>& row) {  // where the mask falls through 0.5
        for (size_t x = 1; x < row.size(); ++x)
            if (row[x] < 0.5f) return int(x);
        return int(row.size());
    };
    for (const char* type : {"matte.select_subject", "matte.select_sky", "matte.select_people", "matte.select_landscape",
                             "matte.select_objects"}) {
        CAPTURE(type);
        Graph g;
        auto* n = dynamic_cast<AutoMaskNode*>(g.addNode(type));
        REQUIRE(n);
        n->params[AutoMaskNode::RefineEdges] = false;
        const std::vector<float> base = mid(n);
        const int edge = crossing(base);
        CHECK(std::abs(edge - w / 2) <= 6);
        n->params[size_t(n->featherParam() + 1)] = 100.0f;
        CHECK(crossing(mid(n)) > edge + 2);
        n->params[size_t(n->featherParam() + 1)] = -100.0f;
        CHECK(crossing(mid(n)) < edge - 2);
        n->params[size_t(n->featherParam() + 1)] = 0.0f;
        // Feather: values between 0 and 1 over a wider stretch.
        n->params[size_t(n->featherParam())] = 100.0f;
        const auto soft = mid(n);
        const auto softCount = [](const std::vector<float>& r) { return std::count_if(r.begin(), r.end(), [](float v) { return v > 0.05f && v < 0.95f; }); };
        CHECK(softCount(soft) > softCount(base) + 4);
        CHECK(n->info().params[size_t(n->featherParam())].name == "Feather");
    }
}

TEST_CASE("Segmentation masks pick classes by their parts, and cache each choice apart") {
    Graph g;
    auto* land = dynamic_cast<AutoMaskNode*>(g.addNode("matte.select_landscape"));
    auto* obj = dynamic_cast<AutoMaskNode*>(g.addNode("matte.select_objects"));
    auto* people = dynamic_cast<AutoMaskNode*>(g.addNode("matte.select_people"));
    REQUIRE((land && obj && people));
    // Water by default: sea (26) among them, not sky (2).
    std::vector<int> c = land->selectedClasses();
    CHECK(std::find(c.begin(), c.end(), 26) != c.end());
    CHECK(std::find(c.begin(), c.end(), 2) == c.end());
    const std::string waterKey = land->maskKey();
    land->params[2] = true;  // Sky
    c = land->selectedClasses();
    CHECK(std::find(c.begin(), c.end(), 2) != c.end());
    CHECK(land->maskKey() != waterKey);
    CHECK(land->maskKey().rfind("scene_c", 0) == 0);
    CHECK(obj->selectedClasses() == std::vector<int>{12});  // person
    CHECK(obj->model().id == std::string("scene"));
    // Face Skin: skin, nose and ears of the face-parsing model's 19 classes.
    CHECK(people->selectedClasses() == std::vector<int>{1, 2, 8, 9});
    CHECK(people->model().classes == 19);
    for (const char* id : {"scene", "face"}) {
        const ml::ModelSpec* spec = ml::findModel(id);
        REQUIRE(spec);
        CHECK(std::string(spec->files[0].url).find("/resolve/") != std::string::npos);  // pinned revision
        CHECK(std::string(spec->files[0].sha256).size() == 64);
    }
}
