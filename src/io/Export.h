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
    enum Format { PNG = 0, JPEG = 1, TIFF = 2, EXR = 3, WEBP = 4, JXL = 5, AVIF = 6 };  // FileFormat's order
    enum Size { Original = 0, LongEdge = 1, Percent = 2 };
    int format = PNG;
    // Bits per channel: 8 or 16 for PNG, TIFF and JPEG XL (AVIF: 8 or 10, stored as 16), 16 (half)
    // or 32 (full float) for OpenEXR.
    int depth = 8;
    // Quality for JPEG and the lossy modes of WebP, JPEG XL and AVIF.
    int jpegQuality = 92;
    // WebP, JPEG XL and AVIF: lossless instead of quality.
    bool lossless = true;
    int sizeMode = Original;
    int longEdge = 2048;    // px, for LongEdge
    int percent = 50;       // for Percent (downscale only)
    // Lightroom's Output Sharpening, applied after resizing: for Screen, Matte Paper or Glossy
    // Paper (0 = off), at Low / Standard / High.
    enum Sharpen { SharpenOff = 0, Screen = 1, Matte = 2, Glossy = 3 };
    int sharpenFor = SharpenOff;
    int sharpenAmount = 1;  // 0 Low, 1 Standard, 2 High
    bool fileOutputs = true;  // also write File Output nodes (single export only)
    // Lightroom's Export Color Space (outspace::Space). OpenEXR stays scene-linear Rec.709;
    // Rec.2100 PQ (HDR) is for PNG, JPEG XL and AVIF, written 16-bit (AVIF 10-bit).
    int colorSpace = 0;
    // Batch naming (Lightroom's File Naming): a filename template, see expandNameTemplate.
    std::string nameTemplate = "{name}";

    const char* extension() const { return formatExtension(FileFormat(format)); }
    // The same file format, size, sharpening and naming (a preset's settings; fileOutputs and
    // where the files go aren't part of one).
    bool sameOutput(const ExportSettings& o) const;
    // The writer options for an image of w x h exported from `source` (for its EXIF).
    SaveOptions saveOptions(const std::string& source, int w, int h) const;
    nlohmann::json toJson() const;
    void fromJson(const nlohmann::json& j);
};

// Lightroom's export presets: named settings. The built-in ones come first; the user's are kept
// in the preferences.
struct ExportPreset {
    std::string name;
    ExportSettings settings;
};
const std::vector<ExportPreset>& builtInExportPresets();

// Resamples to w x h with a Lanczos-3 filter (widened when downscaling, so it also antialiases).
// Colour is filtered premultiplied by alpha, and each pass clamps to the range of the pixels it
// reads, so edges stay sharp without dark or bright halos. Filter linear-light values: with
// srgbEncoded the values are decoded first and re-encoded after.
std::shared_ptr<Image> resizeLanczos(const Image& src, int w, int h, bool srgbEncoded = false);

// Applies the size option (Lanczos-3 in linear light; never enlarges). srgbEncoded: the values
// are sRGB-encoded (legacy projects).
std::shared_ptr<const Image> resizeForExport(const std::shared_ptr<const Image>& img, const ExportSettings& s,
                                             bool srgbEncoded = false);

// Applies the Output Sharpening option (a no-op when off). linear: scene-linear values.
std::shared_ptr<const Image> sharpenForExport(const std::shared_ptr<const Image>& img, const ExportSettings& s,
                                              bool linear);

// Saves a rendered image the way Blender does: display formats (PNG, JPEG, TIFF) get the view
// transform; OpenEXR stays scene-linear (legacy projects' sRGB-encoded values are decoded first).
// Display formats are written in opt.space (see displayInSpace).
bool saveRendered(const std::string& pathU8, const std::shared_ptr<const Image>& scene, const ColorManagement& cm,
                  const SaveOptions& opt, std::string& err);

// The display values of `scene` in an export space. sRGB is colormgmt::displayImage. Wider
// spaces keep the colours sRGB clips: with the Standard view, scene-linear values go straight to
// the space (clipped to its gamut there); other views and legacy projects convert their sRGB
// display values. Rec.2100 PQ keeps highlights above 1 as brighter than reference white (203
// nits), with the exposure but no view transform.
std::shared_ptr<const Image> displayInSpace(const std::shared_ptr<const Image>& scene, const ColorManagement& cm,
                                            int space);

// The file whose metadata exports carry: the first Image Input with a file, or "".
std::string metadataSource(const Graph& g);

// Lightroom's filename templates. Tokens in braces are replaced, case-insensitively:
//   {name} the source's file name without extension, {folder} its folder's name,
//   {seq} the position in the export (from 1), {seq:3} padded to 3 digits,
//   {copy} "Copy 1" for a library virtual copy (else nothing),
//   {date} capture date as YYYY-MM-DD, {year} {month} {day} {hour} {minute} {second}, {time} HHMMSS,
//   {today} the export date, {camera} {make} {lens} {iso} {focal} {aperture} {shutter}.
// The capture date falls back to the file's modification time; other unknown values are empty.
// Characters Windows forbids in file names become "_". Unknown tokens are kept as written.
struct NameSource {
    std::string path;  // the source photo ("" for a single export: {name} is "export")
    int sequence = 1;
    int copy = 0;      // library virtual copy number
};
std::string expandNameTemplate(const std::string& tmpl, const NameSource& src);
// The tokens the template editor offers.
struct NameToken {
    const char* token;
    const char* help;
};
extern const NameToken kNameTokens[];
extern const int kNameTokenCount;

// Output path for one batch source: outDir/<template>.<ext>. Never returns the source itself.
std::string batchOutputPath(const std::string& sourceU8, const std::string& outDirU8, const ExportSettings& s,
                            int sequence = 1, int copy = 0);
// Output paths for a whole batch, numbered in order. A name that is taken gets " (2)", " (3)"...:
// one the template gives twice, a source, or an original already there (a Library photo, or a
// camera file with a source's name, such as the JPEG of a RAW+JPEG pair). Earlier exports are
// replaced.
std::vector<std::string> batchOutputPaths(const std::vector<NameSource>& sources, const std::string& outDirU8,
                                          const ExportSettings& s);

// One file to write. For batches `source` replaces the File of the chosen Image Input node.
struct ExportItem {
    std::string source;  // empty for a single export
    std::string output;  // empty: write only the File Output nodes
    // The library's Export Selected: this photo's own edit (graph JSON, absolute paths), rendered
    // instead of the job's graph; `source` is then only where the EXIF comes from.
    nlohmann::json graph;
    // The photo's library metadata as XMP (library::xmpPacket), written into each of its files.
    std::string xmp;
    // darktable's multi-preset export: more files from the same render, each with its own
    // settings (size, sharpening, format), written after `output`.
    struct Extra {
        std::string output;
        ExportSettings settings;
    };
    std::vector<Extra> extras;
};

// Where an extra preset's files go in a multi-preset export: a subfolder of outDir named after
// the preset, so presets with the same file naming don't overwrite each other.
std::string presetFolder(const std::string& outDirU8, const std::string& presetName);

// Renders at full resolution and saves on a background thread, so the UI stays responsive. The
// graph is copied (as JSON) when the job starts, so editing during an export is safe.
class Exporter {
public:
    ~Exporter();

    // inputNode: the Image Input fed each item's source (batch), or 0. gpu: run GPU nodes on the
    // device (at Full precision: an export is not a preview), as the viewers do.
    void start(const nlohmann::json& graph, std::vector<ExportItem> items, int inputNode, const ExportSettings& s,
               bool gpu = false);
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
    void run(nlohmann::json graph, std::vector<ExportItem> items, int inputNode, ExportSettings s, bool gpu);
    void setStage(const std::string& s);
    void log(const std::string& line);

    std::thread thread_;
    std::atomic<bool> cancel_{false}, busy_{false};
    mutable std::mutex mutex_;
    Progress progress_;
    std::vector<std::string> log_;
};
