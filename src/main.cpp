#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/Version.h"
#include "graph/Evaluator.h"
#include "graph/NodeRegistry.h"
#include "io/Exif.h"
#include "io/Export.h"
#include "io/ImageCache.h"
#include "io/ImageIO.h"
#include "io/Paths.h"
#include "io/ProjectFile.h"
#include "nodes/io/IONodes.h"
#include "nodes/utility/UtilityNodes.h"
#include "ui/App.h"

#ifdef _WIN32
#include <windows.h>
#endif

// NodeLab.exe --render project.nlproj out.png [--depth N] : evaluate at full resolution without a
// window. The extension picks the format (.png, .jpg, .tif, .exr); --depth 16 for 16-bit PNG/TIFF,
// 32 for full-float EXR.
static int renderHeadless(const std::string& project, const std::string& outPath, int depth) {
    Graph g;
    nlohmann::json ui;
    std::string err;
    if (!loadProject(project, g, ui, err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }
    int outId = g.firstOfType(OutputNode::staticInfo().type);
    if (!outId) {
        std::fprintf(stderr, "project has no Output node\n");
        return 1;
    }
    ImageCache cache;
    EvalContext ctx;
    ctx.proxy = false;
    ctx.cache = &cache;
    initContextSize(g, ctx);
    try {
        Evaluator ev;
        ImagePtr img = ev.evaluateDisplay(g, outId, ctx);
        if (!img) {
            std::fprintf(stderr, "Output node produced no image\n");
            return 1;
        }
        SaveOptions opt;
        opt.format = formatFromPath(outPath);
        opt.depth = depth;
        if (opt.format == FileFormat::JPEG) opt.exif = exif::exportBlock(metadataSource(g), img->w, img->h);
        if (!saveRendered(outPath, img, ctx.colorManagement, opt, err)) {
            std::fprintf(stderr, "save failed: %s\n", err.c_str());
            return 1;
        }
        for (const auto& line : writeFileOutputs(g, cache)) std::printf("%s\n", line.c_str());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "evaluation failed: %s\n", e.what());
        return 1;
    }
    return 0;
}

// NodeLab.exe --benchmark project.nlproj [--full] [--runs N] : evaluates the Output node from an
// empty cache N times (after one warm-up run that also loads the images) and prints per-node and
// total median milliseconds. Proxy resolution unless --full.
static int benchmarkHeadless(const std::string& project, bool full, int runs) {
    Graph g;
    nlohmann::json ui;
    std::string err;
    if (!loadProject(project, g, ui, err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }
    int outId = g.firstOfType(OutputNode::staticInfo().type);
    if (!outId) {
        std::fprintf(stderr, "project has no Output node\n");
        return 1;
    }
    ImageCache cache;
    EvalContext ctx;
    ctx.proxy = !full;
    ctx.cache = &cache;
    initContextSize(g, ctx);
    std::unordered_map<int, std::vector<double>> per;
    std::vector<double> totals;
    try {
        for (int r = 0; r <= runs; ++r) {
            Evaluator ev;
            const auto t0 = std::chrono::steady_clock::now();
            ev.evaluateDisplay(g, outId, ctx);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (r == 0) continue;  // warm-up: image decoding
            totals.push_back(ms);
            for (const auto& [id, t] : ev.timings()) per[id].push_back(t);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "evaluation failed: %s\n", e.what());
        return 1;
    }
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v.empty() ? 0.0 : v[v.size() / 2];
    };
    std::vector<std::pair<double, int>> rows;
    for (const auto& [id, v] : per) rows.push_back({median(v), id});
    std::sort(rows.rbegin(), rows.rend());
    std::printf("%s  %dx%d  %s\n", project.c_str(), ctx.defaultW, ctx.defaultH, full ? "full" : "proxy");
    for (const auto& [ms, id] : rows) {
        const Node* n = g.find(id);
        std::string name = n->label.empty() ? n->info().displayName : n->label;
        std::printf("%9.2f ms  %-6d %-28s %s\n", ms, id, name.c_str(), n->info().type.c_str());
    }
    std::printf("%9.2f ms  total (median of %d)\n", median(totals), runs);
    return 0;
}

// NodeLab.exe --batch project.nlproj outDir [--png|--jpg|--tif|--exr] [--depth N] in1 in2 ... : runs
// each source image through the project (fed into its first Image Input) and writes
// outDir/<name>_edit.<ext>. Unset options come from the project's Export settings.
static int batchHeadless(const std::string& project, const std::string& outDir, std::vector<std::string> args) {
    Graph g;
    nlohmann::json ui;
    std::string err;
    if (!loadProject(project, g, ui, err)) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }
    ExportSettings s;
    if (auto e = ui.find("export"); e != ui.end()) s.fromJson(*e);
    std::vector<ExportItem> items;
    std::vector<std::string> sources;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--jpg") s.format = ExportSettings::JPEG;
        else if (a == "--png") s.format = ExportSettings::PNG;
        else if (a == "--tif") s.format = ExportSettings::TIFF;
        else if (a == "--exr") s.format = ExportSettings::EXR;
        else if (a == "--depth" && i + 1 < args.size()) s.depth = std::atoi(args[++i].c_str());
        else if (a.rfind("--", 0) != 0) sources.push_back(a);
    }
    for (const std::string& a : sources) items.push_back({a, batchOutputPath(a, outDir, s)});
    const int input = g.firstOfType(ImageInputNode::staticInfo().type);
    if (!input || items.empty()) {
        std::fprintf(stderr, input ? "no source images given\n" : "project has no Image Input node\n");
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(u8ToPath(outDir), ec);
    Exporter ex;
    ex.start(g.toJson(), std::move(items), input, s);
    ex.wait();
    for (const std::string& line : ex.takeLog()) std::printf("%s\n", line.c_str());
    return ex.progress().failed ? 1 : 0;
}

// The Release exe is a GUI app (-mwindows) with no console; when started from a terminal for a
// command-line mode, reattach to that terminal so printf output shows up.
// NodeLab.exe --list-nodes : every registered node with its pins and params (for keeping GUIDE.md
// in sync). Hidden internal nodes are skipped.
static void listNodes() {
    const NodeRegistry& reg = NodeRegistry::instance();
    for (const std::string& type : reg.types()) {
        const NodeInfo* inf = reg.find(type);
        if (!inf || inf->hidden) continue;
        std::printf("%s | %s | %s\n", inf->category.c_str(), inf->displayName.c_str(), type.c_str());
        for (const PinDesc& p : inf->inputs) std::printf("  in  %s (%s)\n", p.name.c_str(), pinTypeName(p.type));
        for (const PinDesc& p : inf->outputs) std::printf("  out %s (%s)\n", p.name.c_str(), pinTypeName(p.type));
        for (const ParamDesc& p : inf->params) {
            std::string opts;
            for (const std::string& o : p.options) opts += (opts.empty() ? "" : ", ") + o;
            std::printf("  param %s = %s [%g..%g] %s\n", p.name.c_str(), p.def.dump().c_str(), p.min, p.max, opts.c_str());
        }
    }
}

static void attachParentConsole() {
#ifdef _WIN32
    // Output already redirected to a pipe or file: leave it there.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
}

int main(int argc, char** argv) {
    if (argc >= 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v")) {
        attachParentConsole();
        std::printf("NodeLab %s\n", versionString().c_str());
        return 0;
    }
    registerAllNodes();
    if (argc >= 2 && std::string(argv[1]) == "--list-nodes") {
        attachParentConsole();
        listNodes();
        return 0;
    }
    // Note: argv is in the ANSI code page on Windows; fine for ASCII paths in headless mode.
    if (argc >= 4 && std::string(argv[1]) == "--render") {
        attachParentConsole();
        int depth = 8;
        for (int i = 4; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--depth") depth = std::atoi(argv[i + 1]);
        return renderHeadless(argv[2], argv[3], depth);
    }

    if (argc >= 3 && std::string(argv[1]) == "--benchmark") {
        attachParentConsole();
        bool full = false;
        int runs = 5;
        for (int i = 3; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--full") full = true;
            else if (a == "--runs" && i + 1 < argc) runs = std::max(1, std::atoi(argv[++i]));
        }
        return benchmarkHeadless(argv[2], full, runs);
    }

    if (argc >= 5 && std::string(argv[1]) == "--batch") {
        attachParentConsole();
        return batchHeadless(argv[2], argv[3], std::vector<std::string>(argv + 4, argv + argc));
    }

    App::RunOptions opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) opt.screenshot = argv[++i];
        else if (a == "--script" && i + 1 < argc) opt.script = argv[++i];
        else opt.project = a;
    }
    App app;
    return app.run(opt);
}
