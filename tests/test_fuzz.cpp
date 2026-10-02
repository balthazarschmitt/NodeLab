#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "graph/Evaluator.h"
#include "graph/NodeRegistry.h"

// Every node on degenerate sizes (single rows, columns and pixels) and with each numeric param at
// the ends of its range, in both working spaces: nothing may throw or crash, and nodes outside
// Math and Converter (whose maths can legitimately give inf/NaN) must give finite pixels.
namespace {

Node* gradientSource(Graph& g) {
    Node* e = g.addNode("conv.image_expression");
    e->params[0] = "u";
    e->params[1] = "v";
    e->params[2] = "0.5 + 0.4 * sin(u * 12)";
    return e;
}

bool mayBeNonFinite(const std::string& type) {
    return type.rfind("math.", 0) == 0 || type.rfind("conv.", 0) == 0;
}

int countNonFinite(const Value& v, int w, int h) {
    int bad = 0;
    if (ImagePtr img = toImage(v, w, h))
        for (float f : img->px) bad += !std::isfinite(f);
    return bad;
}

}  // namespace

TEST_CASE("every node survives tiny images and extreme params") {
    const int sizes[][2] = {{1, 1}, {1, 7}, {7, 1}, {2, 3}};
    for (const auto& type : NodeRegistry::instance().types()) {
        const NodeInfo* inf = NodeRegistry::instance().find(type);
        if (inf->hidden || inf->outputs.empty()) continue;
        // Variants: defaults, then every numeric param at its hard min, then at its hard max.
        for (int variant = 0; variant < 3; ++variant) {
            for (bool linear : {false, true}) {
                for (const auto& sz : sizes) {
                    CAPTURE(type);
                    CAPTURE(variant);
                    CAPTURE(linear);
                    CAPTURE(sz[0]);
                    CAPTURE(sz[1]);
                    Graph g;
                    Node* src = gradientSource(g);
                    Node* split = g.addNode("color.split_rgb");
                    g.connect(src->id, 0, split->id, 0);
                    Node* n = g.addNode(type);
                    REQUIRE(n);
                    if (variant > 0)
                        for (size_t i = 0; i < inf->params.size(); ++i) {
                            const ParamDesc& d = inf->params[i];
                            if (d.kind != ParamKind::Float && d.kind != ParamKind::Int) continue;
                            // FloatFree's hard range is +-1e6: use the slider ends instead.
                            const bool free = d.hardMax >= 1e6f;
                            const float v = variant == 1 ? (free ? d.min : d.hardMin) : (free ? d.max : d.hardMax);
                            if (d.kind == ParamKind::Int) n->params[i] = int(v);
                            else n->params[i] = v;
                        }
                    for (int i = 0; i < int(inf->inputs.size()); ++i) {
                        const PinType t = inf->inputs[i].type;
                        if (t == PinType::Image) g.connect(src->id, 0, n->id, i);
                        else if (t == PinType::Channel) g.connect(split->id, 1, n->id, i);
                    }
                    EvalContext ctx;
                    ctx.defaultW = sz[0];
                    ctx.defaultH = sz[1];
                    ctx.colorManagement.linear = linear;
                    Evaluator ev;
                    for (int o = 0; o < int(inf->outputs.size()); ++o) {
                        Value v;
                        CHECK_NOTHROW(v = ev.evaluateOutput(g, n->id, o, ctx));
                        if (!mayBeNonFinite(type)) CHECK(countNonFinite(v, sz[0], sz[1]) == 0);
                    }
                }
            }
        }
    }
}

TEST_CASE("every node survives NaN, infinite and huge pixels") {
    // The expression nodes sanitise their results, so feed the nodes hand-made buffers directly:
    // NaN, both infinities, huge and negative values, beyond any range a LUT or histogram indexes.
    const float specials[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -0.5f, 0.0f, 0.5f, 1.0f, 3e9f};
    const int w = 9, h = 6;
    auto img = std::make_shared<Image>(w, h);
    for (size_t i = 0; i < img->px.size(); ++i) img->px[i] = specials[(i * 7 + i / 5) % 10];
    auto ch = std::make_shared<Channel>(Channel::makeSized(w, h));
    for (size_t i = 0; i < ch->data.size(); ++i) ch->data[i] = specials[(i * 3) % 10];
    for (const auto& type : NodeRegistry::instance().types()) {
        const NodeInfo* inf = NodeRegistry::instance().find(type);
        if (inf->hidden || inf->outputs.empty()) continue;
        for (bool linear : {false, true}) {
            CAPTURE(type);
            CAPTURE(linear);
            Graph g;
            Node* n = g.addNode(type);
            REQUIRE(n);
            std::vector<Value> in(inf->inputs.size()), out(inf->outputs.size());
            for (size_t i = 0; i < in.size(); ++i) {
                const PinType t = inf->inputs[i].type;
                if (t == PinType::Image) in[i] = Value(ImagePtr(img));
                else if (t == PinType::Channel) in[i] = Value(ChannelPtr(ch));
                else if (t == PinType::Number) in[i] = Value(specials[i % 10]);
            }
            EvalContext ctx;
            ctx.defaultW = w;
            ctx.defaultH = h;
            ctx.colorManagement.linear = linear;
            CHECK_NOTHROW(n->evaluate(ctx, in, out));
        }
    }
}

TEST_CASE("every node survives malformed params and links from a project file") {
    // What a damaged or hand-edited project can hold: params of the wrong JSON type or far out of
    // range, and links to pins that don't exist.
    const nlohmann::json junk[] = {"text", nlohmann::json::array({1, "x", nullptr}), nlohmann::json::object({{"a", 1}}),
                                   nullptr, -1e9, 1e9, true, -7};
    for (const auto& type : NodeRegistry::instance().types()) {
        const NodeInfo* inf = NodeRegistry::instance().find(type);
        if (inf->hidden || inf->outputs.empty()) continue;
        for (size_t jv = 0; jv < std::size(junk); ++jv) {
            CAPTURE(type);
            CAPTURE(jv);
            nlohmann::json params = nlohmann::json::object();
            for (const auto& d : inf->params)
                if (d.kind != ParamKind::Path && d.kind != ParamKind::SavePath) params[d.name] = junk[jv];
            nlohmann::json gj = {
                {"nodes", nlohmann::json::array({{{"id", 1}, {"type", "conv.image_expression"}},
                                                 {{"id", 2}, {"type", type}, {"params", params}}})},
                {"links", nlohmann::json::array({{{"id", 3}, {"from", {1, 0}}, {"to", {2, 0}}},
                                                 {{"id", 4}, {"from", {1, 5}}, {"to", {2, 1}}},
                                                 {{"id", 5}, {"from", {1, 0}}, {"to", {2, 9}}},
                                                 {{"id", 6}, {"from", {1, -1}}, {"to", {2, -2}}}})}};
            Graph g;
            try {
                g.fromJson(gj);
            } catch (const std::exception&) {
                continue;  // refusing the file is fine
            }
            EvalContext ctx;
            ctx.defaultW = 8;
            ctx.defaultH = 5;
            Evaluator ev;
            // An error the node reports (an expression that doesn't parse) is fine; a crash or a
            // hang is not.
            for (int o = 0; o < int(inf->outputs.size()); ++o) {
                try {
                    ev.evaluateOutput(g, 2, o, ctx);
                } catch (const std::exception&) {
                }
            }
            CHECK_NOTHROW(g.toJson());
        }
    }
}

#include <filesystem>
#include <fstream>
#include <random>

#include "io/ProjectFile.h"

// Whole project files damaged anywhere in their structure: any value replaced by one of another
// type, keys dropped, arrays cut short. Loading may refuse the file, but must not crash, and a
// graph that loads must evaluate and save again.
TEST_CASE("damaged project files load or fail cleanly") {
    namespace fs = std::filesystem;
    const nlohmann::json junk[] = {"text", nlohmann::json::array(), nlohmann::json::array({1, "x", nullptr}),
                                   nlohmann::json::object(), nullptr, -1e9, 3, true, -1};
    const fs::path tmp = fs::temp_directory_path() / "nodelab_fuzz.nlproj";
    std::mt19937 rng(7);
    int loaded = 0, runs = 0;
    for (const char* file : {"examples/demo.nlproj", "examples/effects.nlproj", "examples/infrared_foliage.nlproj",
                             "tests/ui/groupvalues.nlproj"}) {
        std::ifstream in(fs::path(NODELAB_SOURCE_DIR) / file);
        REQUIRE(in);
        const nlohmann::json good = nlohmann::json::parse(in);
        // NODELAB_FUZZ_RUNS sets a longer run for local hunting.
        const int count = getenv("NODELAB_FUZZ_RUNS") ? atoi(getenv("NODELAB_FUZZ_RUNS")) : 150;
        for (int run = 0; run < count; ++run, ++runs) {
            nlohmann::json j = good;
            // Every value in the tree, then damage one or two of them.
            for (int k = 1 + int(rng() % 2); k > 0; --k) {
                std::vector<nlohmann::json*> all;
                std::vector<nlohmann::json*> stack = {&j};
                while (!stack.empty()) {
                    nlohmann::json* v = stack.back();
                    stack.pop_back();
                    all.push_back(v);
                    if (v->is_structured())
                        for (auto& c : *v) stack.push_back(&c);
                }
                nlohmann::json* v = all[1 + rng() % (all.size() - 1)];
                switch (rng() % 3) {
                    case 0: *v = junk[rng() % std::size(junk)]; break;
                    case 1:
                        if (v->is_object() && !v->empty()) v->erase(std::next(v->begin(), long(rng() % v->size())));
                        break;
                    default:
                        if (v->is_array() && !v->empty()) v->erase(v->begin() + long(rng() % v->size()), v->end());
                }
            }
            CAPTURE(file);
            CAPTURE(run);
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                out << j.dump();
            }
            // A crash gives no CAPTURE output: NODELAB_FUZZ_TRACE keeps the last file to replay.
            if (getenv("NODELAB_FUZZ_TRACE")) {
                std::ofstream("fuzz_last.json") << j.dump(1);
                fprintf(stderr, "%s %d\n", file, run);
            }
            Graph g;
            nlohmann::json ui;
            std::string err;
            if (!loadProject(tmp.string(), g, ui, err)) continue;
            ++loaded;
            EvalContext ctx;
            ctx.defaultW = 16;
            ctx.defaultH = 9;
            ctx.scale = 0.05f;
            Evaluator ev;
            for (const auto& [id, n] : g.nodes())
                if (n->info().type == "io.output") try {
                        ev.evaluateOutput(g, id, 0, ctx);
                    } catch (const std::exception&) {
                    }
            CHECK_NOTHROW(g.toJson());
        }
    }
    fs::remove(tmp);
    MESSAGE(loaded << " of " << runs << " damaged projects still loaded");
}
