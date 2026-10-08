// Golden hashes: every node's CPU output, bit for bit, in legacy and scene-linear projects, and the
// example projects rendered at a small proxy size. Legacy projects must render byte-identically
// from release to release, so a changed legacy hash is a bug unless the change is deliberate and
// announced. Linear hashes may change on purpose (a faster approximation): regenerate them and say
// which nodes changed.
//
// REFRACTORY_GOLDEN_WRITE=1 rewrites tests/golden/*.txt from the current build. The hashes depend on
// the toolchain's maths library (the WinLibs GCC build in .toolchain): a different compiler or libm
// may legitimately differ in the last bit.
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "graph/Evaluator.h"
#include "graph/NodeRegistry.h"
#include "io/ImageCache.h"
#include "io/ProjectFile.h"
#include "nodes/io/IONodes.h"

namespace {

namespace fs = std::filesystem;

struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
    void i32(int v) { bytes(&v, sizeof v); }
};

std::string hashValue(const Value& v) {
    Hasher hs;
    if (const auto* img = std::get_if<ImagePtr>(&v.v); img && *img) {
        hs.i32((*img)->w);
        hs.i32((*img)->h);
        hs.bytes((*img)->px.data(), (*img)->px.size() * sizeof(float));
    } else if (const auto* ch = std::get_if<ChannelPtr>(&v.v); ch && *ch) {
        hs.i32((*ch)->w);
        hs.i32((*ch)->h);
        hs.bytes((*ch)->data.data(), (*ch)->data.size() * sizeof(float));
        hs.i32((*ch)->constant);
        hs.bytes(&(*ch)->value, sizeof(float));
    } else if (const auto* f = std::get_if<float>(&v.v)) {
        hs.bytes(f, sizeof(float));
    } else {
        return "empty";
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(hs.h));
    return buf;
}

fs::path goldenDir() { return fs::path(REFRACTORY_SOURCE_DIR) / "tests" / "golden"; }

std::map<std::string, std::string> readGolden(const std::string& name) {
    std::map<std::string, std::string> m;
    std::ifstream in(goldenDir() / name);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const size_t sp = line.rfind(' ');
        if (sp != std::string::npos) m[line.substr(0, sp)] = line.substr(sp + 1);
    }
    return m;
}

bool writing() { return std::getenv("REFRACTORY_GOLDEN_WRITE") != nullptr; }

void writeGolden(const std::string& name, const std::map<std::string, std::string>& m, const char* header) {
    fs::create_directories(goldenDir());
    std::ofstream out(goldenDir() / name, std::ios::binary | std::ios::trunc);
    out << header;
    for (const auto& [k, v] : m) out << k << ' ' << v << '\n';
}

// Compares and reports every mismatch (not only the first), so one run shows what a change touched.
void compare(const std::string& name, const std::map<std::string, std::string>& got, const char* header) {
    if (writing()) {
        writeGolden(name, got, header);
        MESSAGE("wrote tests/golden/" << name << " (" << got.size() << " hashes)");
        return;
    }
    const auto want = readGolden(name);
    REQUIRE_MESSAGE(!want.empty(), "tests/golden/" << name << " is missing: run with REFRACTORY_GOLDEN_WRITE=1");
    int changed = 0, missing = 0;
    for (const auto& [k, v] : got) {
        auto it = want.find(k);
        if (it == want.end()) {
            ++missing;  // a new node or variant: fine, but regenerate to start guarding it
            continue;
        }
        if (it->second != v) {
            ++changed;
            MESSAGE("changed: " << k);
        }
    }
    if (missing) MESSAGE(missing << " entries have no golden hash yet (new nodes or variants)");
    CHECK_MESSAGE(changed == 0, changed << " outputs differ from tests/golden/" << name);
}

// A deterministic 48x32 source with values a little outside 0..1 (as scene-linear and RAW images
// have) and a varying alpha.
Node* source(Graph& g) {
    Node* e = g.addNode("conv.image_expression");
    e->params[0] = "u * 1.3 - 0.08";
    e->params[1] = "0.5 + 0.45 * sin(u * 9 + v * 4)";
    e->params[2] = "v * v";
    if (e->params.size() > 3 && e->info().params[3].kind == ParamKind::Text) e->params[3] = "1 - 0.3 * u * v";
    return e;
}

// Params moved part of the way from their default towards the end of their slider, and switches
// flipped, so nodes whose defaults do nothing (Basic, Exposure) are exercised too.
void push(Node* n) {
    const auto& ps = n->info().params;
    for (size_t i = 0; i < ps.size(); ++i) {
        const ParamDesc& d = ps[i];
        if (d.kind == ParamKind::Float || d.kind == ParamKind::Int) {
            const float def = n->params[i].is_number() ? n->params[i].get<float>() : 0.0f;
            const float to = def < d.max ? d.max : d.min;
            float v = def + 0.35f * (to - def);
            if (d.kind == ParamKind::Int) n->params[i] = int(std::lround(v));
            else n->params[i] = v;
        } else if (d.kind == ParamKind::Bool) {
            n->params[i] = !(n->params[i].is_boolean() && n->params[i].get<bool>());
        }
    }
}

bool skipped(const std::string& type) {
    // Image Input needs a file (the examples cover it); AI masks show a stand-in without models.
    return type == "io.image_input" || type == "matte.select_subject" || type == "matte.select_sky" ||
           type == "util.file_output";
}

std::map<std::string, std::string> nodeHashes(bool linear) {
    std::map<std::string, std::string> out;
    for (const auto& type : NodeRegistry::instance().types()) {
        const NodeInfo* inf = NodeRegistry::instance().find(type);
        if (inf->hidden || skipped(type) || inf->outputs.empty()) continue;
        // default, pushed, and pushed with each option of each enum.
        std::vector<std::pair<std::string, std::pair<int, int>>> variants = {{"default", {-1, -1}}, {"pushed", {-1, -1}}};
        for (int i = 0; i < int(inf->params.size()); ++i)
            if (inf->params[i].kind == ParamKind::Enum)
                for (int o = 0; o < int(inf->params[i].options.size()); ++o)
                    variants.push_back({"p" + std::to_string(i) + "=" + std::to_string(o), {i, o}});
        for (const auto& [vname, en] : variants) {
            Graph g;
            g.colorManagement.linear = linear;
            Node* src = source(g);
            Node* split = g.addNode("color.split_rgb");
            g.connect(src->id, 0, split->id, 0);
            Node* n = g.addNode(type);
            if (vname != "default") push(n);
            if (en.first >= 0) n->params[size_t(en.first)] = en.second;
            for (int i = 0; i < int(inf->inputs.size()); ++i) {
                PinType t = inf->inputs[i].type;
                if (t == PinType::Image) g.connect(src->id, 0, n->id, i);
                else if (t == PinType::Channel) g.connect(split->id, 1, n->id, i);
            }
            EvalContext ctx;
            ctx.defaultW = 48;
            ctx.defaultH = 32;
            ctx.scale = 0.25f;
            ctx.colorManagement = g.colorManagement;
            Evaluator ev;
            for (int o = 0; o < int(inf->outputs.size()); ++o) {
                std::string h;
                try {
                    h = hashValue(toCpu(ev.evaluateOutput(g, n->id, o, ctx)));
                } catch (const std::exception&) {
                    h = "error";
                }
                out[type + " " + vname + " out" + std::to_string(o)] = h;
            }
        }
    }
    return out;
}

const char* kNodeHeader =
    "# Golden hashes of every node's CPU output (tests/test_golden.cpp). Regenerate with\n"
    "# REFRACTORY_GOLDEN_WRITE=1 build\\refractory_tests.exe -tc=\"golden*\" only for deliberate changes.\n";

}  // namespace

TEST_CASE("golden: legacy node outputs are byte-identical") {
    compare("legacy_nodes.txt", nodeHashes(false), kNodeHeader);
}

TEST_CASE("golden: scene-linear node outputs") {
    compare("linear_nodes.txt", nodeHashes(true), kNodeHeader);
}

// The committed examples (all legacy projects) at a small proxy size, through Image Input and the
// whole graph, as the viewer's Result shows them.
TEST_CASE("golden: example projects render byte-identically") {
    std::map<std::string, std::string> got;
    for (const char* file : {"demo.refract", "effects.refract", "infrared_foliage.refract", "m2_showcase.refract"}) {
        Graph g;
        nlohmann::json ui;
        std::string err;
        const fs::path p = fs::path(REFRACTORY_SOURCE_DIR) / "examples" / file;
        REQUIRE_MESSAGE(loadProject(p.string(), g, ui, err), err);
        const int outId = g.firstOfType(OutputNode::staticInfo().type);
        REQUIRE(outId);
        ImageCache cache;
        EvalContext ctx;
        ctx.cache = &cache;
        ctx.proxyEdge = 320;
        initContextSize(g, ctx);
        Evaluator ev;
        ImagePtr img = ev.evaluateDisplay(g, outId, ctx);
        got[std::string("example ") + file] = img ? hashValue(Value(img)) : "empty";
    }
    compare("legacy_examples.txt", got, kNodeHeader);
}
