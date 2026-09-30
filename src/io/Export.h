#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Image.h"

// Output options shared by single exports and batches (File > Export).
struct ExportSettings {
    enum Format { PNG = 0, JPEG = 1 };
    enum Size { Original = 0, LongEdge = 1, Percent = 2 };
    int format = PNG;
    int jpegQuality = 92;
    int sizeMode = Original;
    int longEdge = 2048;    // px, for LongEdge
    int percent = 50;       // for Percent (downscale only)
    bool fileOutputs = true;  // also write File Output nodes (single export only)
    // Batch naming: <source name><suffix>.<ext> in the output folder.
    std::string suffix = "_edit";

    const char* extension() const { return format == JPEG ? ".jpg" : ".png"; }
    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
};

// Applies the size option (box-filter downscale; never enlarges).
std::shared_ptr<const Image> resizeForExport(const std::shared_ptr<const Image>& img, const ExportSettings& s);

// Output path for one batch source: outDir/<stem><suffix><ext>. Never returns the source itself.
std::string batchOutputPath(const std::string& sourceU8, const std::string& outDirU8, const ExportSettings& s);

// One file to write. For batches `source` replaces the File of the chosen Image Input node.
struct ExportItem {
    std::string source;  // empty for a single export
    std::string output;  // empty: write only the File Output nodes
};

// Renders at full resolution and saves on a background thread, so the UI stays responsive. The
// graph is copied (as JSON) when the job starts, so editing during an export is safe.
class Exporter {
public:
    ~Exporter();

    // inputNode: the Image Input fed each item's source (batch), or 0.
    void start(const nlohmann::json& graph, std::vector<ExportItem> items, int inputNode, const ExportSettings& s);
    void cancel() { cancel_ = true; }
    bool busy() const { return busy_; }
    // Blocks until the job ends (tests and the command line).
    void wait();

    struct Progress {
        int done = 0, total = 0;
        std::string stage;  // "Rendering a.jpg", "Saving ..."
        bool cancelled = false;
        int failed = 0;
    };
    Progress progress() const;
    // Log lines since the last call.
    std::vector<std::string> takeLog();

private:
    void run(nlohmann::json graph, std::vector<ExportItem> items, int inputNode, ExportSettings s);
    void setStage(const std::string& s);
    void log(const std::string& line);

    std::thread thread_;
    std::atomic<bool> cancel_{false}, busy_{false};
    mutable std::mutex mutex_;
    Progress progress_;
    std::vector<std::string> log_;
};
