#include "io/Export.h"

#include <algorithm>
#include <cmath>

#include "graph/Evaluator.h"
#include "graph/Graph.h"
#include "io/ImageCache.h"
#include "io/ImageIO.h"
#include "io/Paths.h"
#include "nodes/io/IONodes.h"
#include "nodes/utility/UtilityNodes.h"

nlohmann::json ExportSettings::toJson() const {
    return {{"format", format},     {"jpegQuality", jpegQuality}, {"sizeMode", sizeMode}, {"longEdge", longEdge},
            {"percent", percent},   {"fileOutputs", fileOutputs}, {"suffix", suffix}};
}

void ExportSettings::fromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;
    format = std::clamp(j.value("format", format), 0, 1);
    jpegQuality = std::clamp(j.value("jpegQuality", jpegQuality), 1, 100);
    sizeMode = std::clamp(j.value("sizeMode", sizeMode), 0, 2);
    longEdge = std::clamp(j.value("longEdge", longEdge), 16, 65536);
    percent = std::clamp(j.value("percent", percent), 1, 100);
    fileOutputs = j.value("fileOutputs", fileOutputs);
    suffix = j.value("suffix", suffix);
}

std::shared_ptr<const Image> resizeForExport(const std::shared_ptr<const Image>& img, const ExportSettings& s) {
    if (!img || s.sizeMode == ExportSettings::Original) return img;
    const int edge = std::max(img->w, img->h);
    const int target = s.sizeMode == ExportSettings::LongEdge ? s.longEdge
                                                              : int(std::lround(edge * std::clamp(s.percent, 1, 100) / 100.0));
    return downscaleToFit(img, std::max(1, target));
}

std::string batchOutputPath(const std::string& sourceU8, const std::string& outDirU8, const ExportSettings& s) {
    const auto src = u8ToPath(sourceU8);
    auto name = src.stem();
    auto out = u8ToPath(outDirU8) / u8ToPath(pathToU8(name) + s.suffix + s.extension());
    // An empty suffix into the source folder would overwrite the original (same name and format).
    std::error_code ec;
    if (std::filesystem::equivalent(out, src, ec) || out.lexically_normal() == src.lexically_normal())
        out = u8ToPath(outDirU8) / u8ToPath(pathToU8(name) + "_edit" + s.extension());
    return pathToU8(out);
}

Exporter::~Exporter() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

void Exporter::start(const nlohmann::json& graph, std::vector<ExportItem> items, int inputNode, const ExportSettings& s) {
    if (thread_.joinable()) thread_.join();  // a finished job's thread
    cancel_ = false;
    busy_ = true;
    {
        std::lock_guard lock(mutex_);
        progress_ = {};
        progress_.total = int(items.size());
    }
    thread_ = std::thread([this, graph, items = std::move(items), inputNode, s]() mutable {
        run(std::move(graph), std::move(items), inputNode, std::move(s));
    });
}

void Exporter::wait() {
    if (thread_.joinable()) thread_.join();
}

Exporter::Progress Exporter::progress() const {
    std::lock_guard lock(mutex_);
    return progress_;
}

std::vector<std::string> Exporter::takeLog() {
    std::lock_guard lock(mutex_);
    return std::exchange(log_, {});
}

void Exporter::setStage(const std::string& s) {
    std::lock_guard lock(mutex_);
    progress_.stage = s;
}

void Exporter::log(const std::string& line) {
    std::lock_guard lock(mutex_);
    log_.push_back(line);
}

void Exporter::run(nlohmann::json graphJson, std::vector<ExportItem> items, int inputNode, ExportSettings s) {
    Graph g;
    try {
        g.fromJson(graphJson);
    } catch (const std::exception& e) {
        log(std::string("Export failed: ") + e.what());
        busy_ = false;
        return;
    }
    const int outId = g.firstOfType(OutputNode::staticInfo().type);
    const bool batch = inputNode != 0;
    int failed = 0;
    for (size_t i = 0; i < items.size() && !cancel_; ++i) {
        const ExportItem& item = items[i];
        const std::string outName = pathToU8(u8ToPath(item.output).filename());
        // A fresh cache per item: batches would otherwise keep every source's preview in memory.
        ImageCache cache;
        EvalContext ctx;
        ctx.proxy = false;
        ctx.cache = &cache;
        ctx.cancel = &cancel_;
        try {
            if (batch) {
                Node* in = g.find(inputNode);
                if (!in) throw std::runtime_error("the batch Image Input node is gone");
                in->params[0] = item.source;
                setStage("Loading " + pathToU8(u8ToPath(item.source).filename()));
                std::string err;
                ImagePtr src = cache.get(item.source, false, &err);
                if (!src) throw std::runtime_error(err.empty() ? "could not load " + item.source : err);
                // Size from this source, not whichever Image Input happens to come first.
                ctx.defaultW = src->w;
                ctx.defaultH = src->h;
                ctx.scale = 1.0f;
            } else {
                initContextSize(g, ctx);
            }
            Evaluator ev;
            if (!item.output.empty()) {
                setStage("Rendering " + outName);
                ImagePtr img = outId ? ev.evaluateDisplay(g, outId, ctx) : nullptr;
                if (!img) throw std::runtime_error("the Output node has no input");
                img = resizeForExport(img, s);
                if (cancel_) break;
                setStage("Saving " + outName);
                std::string err;
                if (!saveImage(item.output, *img, err, s.jpegQuality)) throw std::runtime_error(err);
                log("Wrote " + item.output + " (" + std::to_string(img->w) + " x " + std::to_string(img->h) + ")");
            }
            // File Output nodes have fixed paths, so a batch would overwrite them on every item.
            if (!batch && (s.fileOutputs || item.output.empty())) {
                setStage("Writing File Outputs");
                for (const std::string& line : writeFileOutputs(g, ev, ctx)) log(line);
            }
        } catch (const EvalCancelled&) {
            break;
        } catch (const std::exception& e) {
            ++failed;
            log("Failed " + (batch ? item.source : outName) + ": " + e.what());
        }
        std::lock_guard lock(mutex_);
        progress_.done = int(i + 1);
        progress_.failed = failed;
    }
    {
        std::lock_guard lock(mutex_);
        progress_.cancelled = cancel_;
        progress_.stage = cancel_ ? "Cancelled" : "Done";
    }
    if (cancel_) log("Cancelled");
    busy_ = false;
}
