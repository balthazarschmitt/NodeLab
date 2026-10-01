#include "io/Export.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

#include "core/ColorMath.h"
#include "core/Parallel.h"
#include "gpu/Device.h"
#include "graph/Evaluator.h"
#include "graph/Graph.h"
#include "io/Exif.h"
#include "io/ImageCache.h"
#include "io/ImageIO.h"
#include "io/Paths.h"
#include "nodes/io/IONodes.h"
#include "nodes/utility/UtilityNodes.h"

SaveOptions ExportSettings::saveOptions(const std::string& source, int w, int h) const {
    SaveOptions o;
    o.format = FileFormat(format);
    o.depth = depth;
    o.jpegQuality = jpegQuality;
    if (format == JPEG) o.exif = exif::exportBlock(source, w, h);
    return o;
}

nlohmann::json ExportSettings::toJson() const {
    return {{"format", format},     {"depth", depth},           {"jpegQuality", jpegQuality},
            {"sizeMode", sizeMode}, {"longEdge", longEdge},     {"percent", percent},
            {"fileOutputs", fileOutputs}, {"suffix", suffix}};
}

void ExportSettings::fromJson(const nlohmann::json& j) {
    if (!j.is_object()) return;
    format = std::clamp(j.value("format", format), 0, 3);
    depth = std::clamp(j.value("depth", depth), 8, 32);
    jpegQuality = std::clamp(j.value("jpegQuality", jpegQuality), 1, 100);
    sizeMode = std::clamp(j.value("sizeMode", sizeMode), 0, 2);
    longEdge = std::clamp(j.value("longEdge", longEdge), 16, 65536);
    percent = std::clamp(j.value("percent", percent), 1, 100);
    fileOutputs = j.value("fileOutputs", fileOutputs);
    suffix = j.value("suffix", suffix);
}

namespace {

float lanczos3(double x) {
    x = std::abs(x);
    if (x < 1e-8) return 1.0f;
    if (x >= 3.0) return 0.0f;
    const double px = std::numbers::pi * x;
    return float(3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px));
}

// Per output sample: the first source sample and `stride` weights (zero-padded), normalised.
struct Taps {
    std::vector<int> first;
    std::vector<float> w;
    int stride = 0;
};

Taps lanczosTaps(int n, int m) {
    const double scale = double(m) / n;
    const double fs = std::min(scale, 1.0);  // downscaling widens the kernel by 1/scale
    const double support = 3.0 / fs;
    Taps t;
    t.stride = int(std::ceil(support * 2)) + 2;
    t.first.resize(m);
    t.w.assign(size_t(m) * t.stride, 0.0f);
    for (int i = 0; i < m; ++i) {
        const double center = (i + 0.5) / scale;  // in source pixels
        const int lo = std::max(0, int(std::floor(center - support)));
        const int hi = std::min(n - 1, int(std::ceil(center + support)));
        t.first[i] = lo;
        float* w = &t.w[size_t(i) * t.stride];
        double sum = 0;
        for (int j = lo; j <= hi && j - lo < t.stride; ++j) sum += w[j - lo] = lanczos3((j + 0.5 - center) * fs);
        // Taps cut off at the image edge are dropped, so renormalise.
        if (sum != 0)
            for (int k = 0; k < t.stride; ++k) w[k] = float(w[k] / sum);
    }
    return t;
}

}  // namespace

std::shared_ptr<Image> resizeLanczos(const Image& src, int w, int h, bool srgbEncoded) {
    w = std::max(1, w);
    h = std::max(1, h);
    // Linear light, colour premultiplied by alpha.
    Image in(src.w, src.h);
    parallelFor(src.h, [&](int y) {
        for (int x = 0; x < src.w; ++x) {
            const size_t i = size_t(y) * src.w + x;
            const float* s = src.pixel(i);
            float* d = in.pixel(i);
            const float a = std::clamp(s[3], 0.0f, 1.0f);
            for (int c = 0; c < 3; ++c) d[c] = (srgbEncoded ? colormath::srgbToLinear(s[c]) : s[c]) * a;
            d[3] = a;
        }
    });
    const Taps tx = lanczosTaps(src.w, w), ty = lanczosTaps(src.h, h);

    // Horizontal pass into src.h rows of w pixels.
    Image mid(w, src.h);
    parallelFor(src.h, [&](int y) {
        const float* row = in.pixel(size_t(y) * src.w);
        for (int x = 0; x < w; ++x) {
            const float* wt = &tx.w[size_t(x) * tx.stride];
            float acc[4] = {0, 0, 0, 0}, lo[4], hi[4];
            for (int c = 0; c < 4; ++c) lo[c] = INFINITY, hi[c] = -INFINITY;
            for (int k = 0; k < tx.stride && tx.first[x] + k < src.w; ++k) {
                if (wt[k] == 0.0f) continue;
                const float* p = row + size_t(tx.first[x] + k) * 4;
                for (int c = 0; c < 4; ++c) {
                    acc[c] += wt[k] * p[c];
                    lo[c] = std::min(lo[c], p[c]);
                    hi[c] = std::max(hi[c], p[c]);
                }
            }
            float* d = mid.pixel(size_t(y) * w + x);
            for (int c = 0; c < 4; ++c) d[c] = std::clamp(acc[c], lo[c], hi[c]);
        }
    });

    // Vertical pass, a whole output row at a time so the reads run along rows.
    auto out = std::make_shared<Image>(w, h);
    parallelFor(h, [&](int y) {
        const float* wt = &ty.w[size_t(y) * ty.stride];
        std::vector<float> acc(size_t(w) * 4, 0.0f), lo(size_t(w) * 4, INFINITY), hi(size_t(w) * 4, -INFINITY);
        for (int k = 0; k < ty.stride && ty.first[y] + k < src.h; ++k) {
            if (wt[k] == 0.0f) continue;
            const float* row = mid.pixel(size_t(ty.first[y] + k) * w);
            for (size_t i = 0; i < size_t(w) * 4; ++i) {
                acc[i] += wt[k] * row[i];
                lo[i] = std::min(lo[i], row[i]);
                hi[i] = std::max(hi[i], row[i]);
            }
        }
        for (int x = 0; x < w; ++x) {
            float* d = out->pixel(size_t(y) * w + x);
            const size_t i = size_t(x) * 4;
            const float a = std::clamp(acc[i + 3], lo[i + 3], hi[i + 3]);
            d[3] = a;
            for (int c = 0; c < 3; ++c) {
                const float v = std::clamp(acc[i + c], lo[i + c], hi[i + c]);
                const float un = a > 0.0f ? v / a : 0.0f;
                d[c] = srgbEncoded ? colormath::linearToSrgb(un) : un;
            }
        }
    });
    return out;
}

std::shared_ptr<const Image> resizeForExport(const std::shared_ptr<const Image>& img, const ExportSettings& s,
                                             bool srgbEncoded) {
    if (!img || s.sizeMode == ExportSettings::Original) return img;
    const int edge = std::max(img->w, img->h);
    const int target = std::max(1, s.sizeMode == ExportSettings::LongEdge
                                       ? s.longEdge
                                       : int(std::lround(edge * std::clamp(s.percent, 1, 100) / 100.0)));
    if (edge <= target) return img;  // never enlarge
    const double scale = double(target) / edge;
    return resizeLanczos(*img, std::max(1, int(std::lround(img->w * scale))), std::max(1, int(std::lround(img->h * scale))),
                         srgbEncoded);
}

bool saveRendered(const std::string& pathU8, const std::shared_ptr<const Image>& scene, const ColorManagement& cm,
                  const SaveOptions& opt, std::string& err) {
    if (!scene) {
        err = "nothing to save";
        return false;
    }
    if (opt.format != FileFormat::EXR) return writeImage(pathU8, *colormgmt::displayImage(scene, cm), opt, err);
    if (cm.linear) return writeImage(pathU8, *scene, opt, err);
    // Legacy projects hold sRGB-encoded values; EXR stores linear light.
    Image lin(scene->w, scene->h);
    parallelFor(scene->h, [&](int y) {
        for (int x = 0; x < scene->w; ++x) {
            const size_t i = size_t(y) * scene->w + x;
            const float* s = scene->pixel(i);
            float* d = lin.pixel(i);
            for (int c = 0; c < 3; ++c) d[c] = colormath::srgbToLinear(s[c]);
            d[3] = s[3];
        }
    });
    return writeImage(pathU8, lin, opt, err);
}

std::string metadataSource(const Graph& g) {
    for (const auto& [id, n] : g.nodes())
        if (n->info().type == ImageInputNode::staticInfo().type && !n->paramS(0).empty()) return n->paramS(0);
    return {};
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

void Exporter::start(const nlohmann::json& graph, std::vector<ExportItem> items, int inputNode, const ExportSettings& s,
                     bool gpu) {
    if (thread_.joinable()) thread_.join();  // a finished job's thread
    cancel_ = false;
    busy_ = true;
    {
        std::lock_guard lock(mutex_);
        progress_ = {};
        progress_.total = int(items.size());
    }
    thread_ = std::thread([this, graph, items = std::move(items), inputNode, s, gpu]() mutable {
        run(std::move(graph), std::move(items), inputNode, std::move(s), gpu);
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

void Exporter::run(nlohmann::json graphJson, std::vector<ExportItem> items, int inputNode, ExportSettings s, bool gpu) {
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
        ctx.colorManagement = g.colorManagement;
        ctx.gpu = gpu && gpu::available();
        ctx.gpuHalf = false;
        try {
            if (batch) {
                Node* in = g.find(inputNode);
                if (!in) throw std::runtime_error("the batch Image Input node is gone");
                // As if chosen in the UI: a RAW batch from a JPEG project gets the RAW defaults.
                static_cast<ImageInputNode&>(*in).chooseFile(item.source);
                setStage("Loading " + pathToU8(u8ToPath(item.source).filename()));
                std::string err;
                ImagePtr src = cache.get(item.source, false, &err,
                                         static_cast<const ImageInputNode&>(*in).decode(ctx.linear()));
                if (!src) throw std::runtime_error(err.empty() ? "could not load " + item.source : err);
                // Size from this source, not whichever Image Input happens to come first.
                ctx.defaultW = src->w;
                ctx.defaultH = src->h;
                ctx.scale = 1.0f;
            } else {
                initContextSize(g, ctx);
            }
            Evaluator ev;
            // File Output nodes have fixed paths, so a batch would overwrite them on every item.
            const bool fileOutputs = !batch && (s.fileOutputs || item.output.empty());
            // A full-resolution render needs only the outputs still to be read: dropping the rest
            // keeps a big photo's peak memory to a few images instead of one per node. File
            // Outputs read the graph again afterwards, so they keep everything.
            ev.releaseIntermediates = !fileOutputs;
            if (!item.output.empty()) {
                setStage("Rendering " + outName);
                ImagePtr img;
                if (outId) {
                    // The device is held while evaluating only (the previews wait meanwhile), not
                    // while resizing and saving.
                    std::optional<gpu::Scope> device;
                    if (ctx.gpu) device.emplace();
                    img = ev.evaluateDisplay(g, outId, ctx);
                }
                if (!img) throw std::runtime_error("the Output node has no input");
                // Before the view transform: resampling in scene light.
                img = resizeForExport(img, s, !ctx.linear());
                if (cancel_) break;
                setStage("Saving " + outName);
                std::string err;
                const SaveOptions opt = s.saveOptions(batch ? item.source : metadataSource(g), img->w, img->h);
                if (!saveRendered(item.output, img, ctx.colorManagement, opt, err)) throw std::runtime_error(err);
                log("Wrote " + item.output + " (" + std::to_string(img->w) + " x " + std::to_string(img->h) + ")");
            }
            if (fileOutputs) {
                setStage("Writing File Outputs");
                for (const std::string& line : writeFileOutputs(g, ev, ctx)) log(line);
            }
            if (ev.gpuFallbacks)
                log(std::to_string(ev.gpuFallbacks) + " nodes ran on the CPU instead: " + ev.lastGpuError);
        } catch (const EvalCancelled&) {
            break;
        } catch (const std::exception& e) {
            ++failed;
            log("Failed " + (batch ? item.source : outName) + ": " + e.what());
        }
        if (ctx.gpu) {
            // A full-resolution image's textures are hundreds of MB: keep only a preview's worth.
            gpu::Scope device;
            gpu::trimPool(size_t(128) << 20);
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
