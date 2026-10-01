#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/ColorManagement.h"
#include "core/Image.h"
#include "io/ImageWrite.h"

class Graph;

// Output options shared by single exports and batches (File > Export).
struct ExportSettings {
    enum Format { PNG = 0, JPEG = 1, TIFF = 2, EXR = 3 };  // FileFormat's order
    enum Size { Original = 0, LongEdge = 1, Percent = 2 };
    int format = PNG;
    // Bits per channel: 8 or 16 for PNG and TIFF, 16 (half) or 32 (full float) for OpenEXR.
    int depth = 8;
    int jpegQuality = 92;
    int sizeMode = Original;
    int longEdge = 2048;    // px, for LongEdge
    int percent = 50;       // for Percent (downscale only)
    bool fileOutputs = true;  // also write File Output nodes (single export only)
    // Batch naming: <source name><suffix>.<ext> in the output folder.
    std::string suffix = "_edit";

    const char* extension() const { return formatExtension(FileFormat(format)); }
    // The writer options for an image of w x h exported from `source` (for its EXIF).
    SaveOptions saveOptions(const std::string& source, int w, int h) const;
    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
};

// Resamples to w x h with a Lanczos-3 filter (widened when downscaling, so it also antialiases).
// Colour is filtered premultiplied by alpha, and each pass clamps to the range of the pixels it
// reads, so edges stay sharp without dark or bright halos. Filter linear-light values: with
// srgbEncoded the values are decoded first and re-encoded after.
std::shared_ptr<Image> resizeLanczos(const Image& src, int w, int h, bool srgbEncoded = false);

// Applies the size option (Lanczos-3 in linear light; never enlarges). srgbEncoded: the values
// are sRGB-encoded (legacy projects).
std::shared_ptr<const Image> resizeForExport(const std::shared_ptr<const Image>& img, const ExportSettings& s,
                                             bool srgbEncoded = false);

// Saves a rendered image the way Blender does: display formats (PNG, JPEG, TIFF) get the view
// transform; OpenEXR stays scene-linear (legacy projects' sRGB-encoded values are decoded first).
bool saveRendered(const std::string& pathU8, const std::shared_ptr<const Image>& scene, const ColorManagement& cm,
                  const SaveOptions& opt, std::string& err);

// The file whose metadata exports carry: the first Image Input with a file, or "".
std::string metadataSource(const Graph& g);

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
