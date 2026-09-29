#include <cstdio>
#include <string>

#include "graph/Evaluator.h"
#include "graph/NodeRegistry.h"
#include "io/ImageCache.h"
#include "io/ImageIO.h"
#include "io/ProjectFile.h"
#include "nodes/io/IONodes.h"
#include "nodes/utility/UtilityNodes.h"
#include "ui/App.h"

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

int main(int argc, char** argv) {
    registerAllNodes();
    // Note: argv is in the ANSI code page on Windows; fine for ASCII paths in headless mode.
    if (argc >= 4 && std::string(argv[1]) == "--render") return renderHeadless(argv[2], argv[3]);

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
