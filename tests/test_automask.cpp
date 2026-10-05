// AI masks (Select Subject, Select Sky): the model downloads' checksums, the nodes without their
// models, the Add Mask recipes, and (opt-in, NODELAB_ML_REAL=1) the installed models themselves.
#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>

#include "graph/Graph.h"
#include "graph/Recipes.h"
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
    for (const char* type : {"matte.select_subject", "matte.select_sky"}) {
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
    for (const char* type : {"matte.select_sky", "matte.select_subject"}) {
        Graph g;
        auto* n = dynamic_cast<AutoMaskNode*>(g.addNode(type));
        REQUIRE(n);
        if (!ml::available(n->model().id)) continue;
        // The node's input size is the model's.
        std::string err;
        const std::vector<int64_t> shape = ml::inputShape(n->model().id, err);
        REQUIRE(shape.size() == 4);
        CHECK(shape[2] == n->model().height);
        CHECK(shape[3] == n->model().width);
        if (std::string(type) != "matte.select_sky") continue;
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
        for (int y = 0; y < 200; ++y)  // a dark building in the sky
            for (int x = 100; x < 160; ++x)
                for (int k = 0; k < 3; ++k) other->pixel(size_t(y) * 300 + x)[k] = 0.1f;
        const int gen = AutoMaskNode::resultGeneration();
        CHECK(n->infer(*other, false, true, probs, pw, ph, err, nullptr) == AutoMaskNode::Inferred::Pending);
        AutoMaskNode::waitForRuns();
        CHECK(AutoMaskNode::resultGeneration() > gen);
        CHECK(n->infer(*other, false, true, probs, pw, ph, err, nullptr) == AutoMaskNode::Inferred::Ready);
    }
}
