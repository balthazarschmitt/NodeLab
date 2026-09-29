#include <cstdio>
#include <string>

#include "core/Version.h"
#include "graph/Evaluator.h"
#include "graph/NodeRegistry.h"
#include "io/ImageCache.h"
#include "io/ImageIO.h"
#include "io/ProjectFile.h"
#include "nodes/io/IONodes.h"
#include "nodes/utility/UtilityNodes.h"
#include "ui/App.h"

#ifdef _WIN32
#include <windows.h>
#endif

// NodeLab.exe --render project.nlproj out.png  : evaluate at full resolution without a window.
static int renderHeadless(const std::string& project, const std::string& outPath) {
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
        if (!saveImage(outPath, *img, err)) {
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
        return renderHeadless(argv[2], argv[3]);
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
