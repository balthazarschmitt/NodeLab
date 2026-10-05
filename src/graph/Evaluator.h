#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/Parallel.h"  // EvalCancelled
#include "graph/Graph.h"

namespace gpu {
class Timer;
}

// Pull-based graph evaluator with a per-node output cache. A node is recomputed only when
// its signature (type + params + upstream signatures + resolution mode) changes.
using NodePath = std::vector<int>;  // group ids from the root, then the node id

class Evaluator {
public:
    // Like evaluateDisplay, but the node may sit inside (nested) groups.
    ImagePtr evaluateDisplayPath(const Graph& g, const NodePath& path, EvalContext& ctx, int pin = 0,
                                 Value* onGpu = nullptr);
    // Values arriving at a node's inputs (fallback params applied), evaluating upstream as needed.
    std::vector<Value> gatherInputs(const Graph& g, int nodeId, EvalContext& ctx);

    // Value a node should show in a preview: for sink nodes (no outputs, e.g. Output) the value
    // arriving at input 0, otherwise output pin `pin`. Converted to an image. With `onGpu`, a
    // value on the GPU device is returned there instead, without a download (the image is null).
    ImagePtr evaluateDisplay(const Graph& g, int nodeId, EvalContext& ctx, int pin = 0, Value* onGpu = nullptr);
    Value evaluateOutput(const Graph& g, int nodeId, int pin, EvalContext& ctx);

    // Cache levels. Each keeps its own results, so moving between them throws no work away: the
    // preview; a draft at half the preview's resolution while a slow graph is being dragged
    // (progressive refinement, as in darktable and Lightroom); and regions for zoomed-in viewing.
    enum Level { Preview, Draft, Region, kLevels };
    void setLevel(Level l) { level_ = l; }
    Level level() const { return level_; }

    // The part of a top-level node's output a zoomed-in viewer shows, evaluated at ctx.scale
    // (see initRegionContext) from full-resolution sources, without computing the rest: each node
    // runs on the region its consumers need, grown by its Node::roiPadding. The region, in 0..1
    // of the output, is rounded out to whole pixels. The preview of the same graph must have been
    // evaluated first (it tells which values are sized, and holds the global statistics).
    // Returns null when the graph can't be evaluated by region; the viewer then keeps the preview.
    struct RegionResult {
        ImagePtr image;
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // where the image sits, in 0..1 of the whole output
    };
    std::optional<RegionResult> evaluateRegion(const Graph& g, int nodeId, int pin, EvalContext& ctx, float u0, float v0,
                                               float u1, float v1);

    // Drop cache entries for nodes that no longer exist.
    void prune(const Graph& g);
    void clearCache() {
        for (auto& c : cache_) c.clear();
    }
    void clearLevel(Level l) { cache_[l].clear(); }
    // Memory held by a level's cached results.
    size_t bytes(Level l) const;

    // One-off renders (exports): drop each node's result once every node reading it has run, so a
    // full-resolution render holds only the images still needed instead of one per node.
    bool releaseIntermediates = false;
    // Benchmarks: wait for the device before and after each GPU node, so its time is its own.
    // Without it, a node that reads results back (Normalize) also waits for all the work queued
    // before it, and some drivers count that wait in its timer query.
    bool syncTimings = false;

    int recomputeCount = 0;  // nodes actually evaluated (for tests / stats)
    // GPU compositing (EvalContext::gpu): nodes run on the GPU, and ones that failed there (out of
    // GPU memory, a driver rejecting a shader) and ran on the CPU instead, with the last reason.
    int gpuRuns = 0, gpuFallbacks = 0;
    std::string lastGpuError;

    // Milliseconds the node's own evaluate() took when it last ran (upstream work excluded), like
    // Blender's compositor Node Timings. -1 if the node has no cached result.
    double nodeMs(int nodeId) const;
    // Timings of every cached node in the graph last evaluated (top level only; a group's time
    // includes its inner nodes).
    std::unordered_map<int, double> timings() const;
    // Which of those ran on the GPU.
    std::unordered_map<int, bool> gpuNodes() const;

private:
    struct Entry {
        size_t sig = 0;
        std::vector<Value> outs;
        double ms = 0;
        std::vector<float> stats;  // global statistics recorded by a preview run (EvalContext::statsOut)
        // outs moved to the other device (uploaded for GPU nodes, downloaded for CPU ones), made
        // once however many nodes read them.
        std::vector<Value> alt;
        bool gpu = false;  // ran on the GPU
        std::shared_ptr<gpu::Timer> timer;  // its device time (GPU nodes)
        double time() const;                 // ms, or the device time if longer
    };
    // Whether nodes may run on the GPU at this level (whole images only; regions stay on the CPU).
    bool gpuLevel(const EvalContext& ctx) const;
    void materializeShared(const Graph& g, const std::vector<const Link*>& from, std::vector<Value>& inputs);
    // Output `pin` of `up` on the GPU or CPU, from its alt cache.
    Value converted(Entry& up, int pin, bool toGpu, const EvalContext& ctx);
    size_t ensure(const Graph& g, int nodeId, EvalContext& ctx, std::unordered_map<int, size_t>& pass);
    // Runs a node (or its bypass when muted) and stores the result in e.
    void run(const Graph& g, Node& n, EvalContext& ctx, std::vector<Value>& inputs, Entry& e, size_t sig, bool gpu);
    std::string baseKey(const Node& n, const EvalContext& ctx) const;

    Level level_ = Preview;
    std::unordered_map<int, Entry> cache_[kLevels];
    std::unordered_map<int, int> readers_;  // releaseIntermediates: reads of each node still to come
};

// Sets the context's colour management from the root graph, and its size and scale from the first
// Image Input that loads.
void initContextSize(const Graph& g, EvalContext& ctx);
// For Evaluator::evaluateRegion: full-resolution sources scaled by `scale`, with the working size
// of the first Image Input's level at that scale. False when no Image Input has loaded.
bool initRegionContext(const Graph& g, EvalContext& ctx, float scale);

// Runs evaluation on a background thread. Newer submissions replace queued ones; the UI keeps
// showing the last completed result meanwhile. A running job is normally left to finish, so a
// slider drag keeps producing intermediate results; `preempt` cancels it instead, for edits whose
// result is the one that matters (a finished gesture, undo, a typed value).
class AsyncEvaluator {
public:
    // A zoomed-in view: the part of a top-level node's output it shows, evaluated sharper than the
    // preview from the full-resolution sources (Evaluator::evaluateRegion).
    struct Detail {
        int tag = 0;  // the caller's, copied to the tile (which view it is for)
        int node = 0, pin = 0;
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // visible part, in 0..1 of the output
        float screenW = 0;                     // screen pixels across the whole output's width
    };
    struct Options {
        int proxyEdge = 1280;  // long edge of the preview sources (ImageCache::kProxyEdge)
        // Half the proxy edge, in its own cache level: quick feedback while dragging a slider on a
        // slow graph, refined by the next full submission (progressive refinement).
        bool draft = false;
        std::vector<Detail> details;  // evaluated after the preview (not for drafts)
        // Blender's compositor Device and Precision (EvalContext::gpu, gpuHalf).
        bool gpu = false;
        bool gpuHalf = true;
    };
    struct Tile {
        int tag = 0;        // Detail::tag
        ImagePtr image;     // null: the preview is already as sharp, or the graph can't do regions
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // where it sits, in 0..1 of the output
    };
    struct Result {
        std::vector<ImagePtr> images;    // one per submitted target (empty: only tiles)
        // Targets still on the GPU device, not downloaded (their image is then null).
        std::vector<Value> gpuImages;
        std::vector<std::string> errors;  // one per submitted target (empty = ok)
        double ms = 0;
        uint64_t generation = 0;
        bool draft = false;
        std::unordered_map<int, double> nodeMs;  // top-level node timings (see Evaluator::timings)
        std::unordered_map<int, bool> nodeGpu;   // top-level nodes that ran on the GPU
        int gpuFallbacks = 0;                    // nodes so far that failed on the GPU (Evaluator::gpuFallbacks)
        std::string gpuError;                    // the last reason
        // Details, delivered after the images (merged into them if those weren't polled yet).
        std::vector<Tile> tiles;
        bool tilesDone = false;
        // Why the details failed (out of memory, most likely): the views keep the preview.
        std::string tilesError;
    };

    explicit AsyncEvaluator(ImageCache& cache);
    ~AsyncEvaluator();
    AsyncEvaluator(const AsyncEvaluator&) = delete;
    AsyncEvaluator& operator=(const AsyncEvaluator&) = delete;

    // Evaluates several display targets in one pass (they share the node cache).
    // pins[i]: which output of targets[i] to show (defaults to 0).
    void submit(nlohmann::json graphJson, std::vector<NodePath> targets, std::vector<int> pins, bool preempt,
                Options options);
    void submit(nlohmann::json graphJson, std::vector<NodePath> targets, std::vector<int> pins = {}, bool preempt = false) {
        submit(std::move(graphJson), std::move(targets), std::move(pins), preempt, Options());
    }
    // Cancels the running job if a newer one is queued (e.g. when a drag ends: the running job
    // shows an intermediate value and the queued one the final value). Nodes it already finished
    // stay cached.
    void preempt();
    // Returns a result once per completed job.
    std::optional<Result> poll();
    bool busy() const { return busy_.load(); }

private:
    struct Job {
        nlohmann::json graph;
        std::vector<NodePath> targets;
        std::vector<int> pins;
        Options options;
        uint64_t generation = 0;
    };
    void run();
    std::vector<Tile> evaluateDetails(const Graph& g, const Job& job, EvalContext& ctx);
    // Drops cached levels beyond the memory budget.
    void trimCache();

    ImageCache& cache_;
    Evaluator evaluator_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Job> pending_;
    std::optional<Result> done_;
    uint64_t nextGen_ = 1;
    bool quit_ = false;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> busy_{false};
    bool inDetails_ = false;  // a new submission cancels detail work (guarded by mutex_)
};
