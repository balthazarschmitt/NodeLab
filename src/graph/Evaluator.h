#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/Parallel.h"  // EvalCancelled
#include "graph/Graph.h"

// Pull-based graph evaluator with a per-node output cache. A node is recomputed only when
// its signature (type + params + upstream signatures + resolution mode) changes.
using NodePath = std::vector<int>;  // group ids from the root, then the node id

class Evaluator {
public:
    // Like evaluateDisplay, but the node may sit inside (nested) groups.
    ImagePtr evaluateDisplayPath(const Graph& g, const NodePath& path, EvalContext& ctx, int pin = 0);
    // Values arriving at a node's inputs (fallback params applied), evaluating upstream as needed.
    std::vector<Value> gatherInputs(const Graph& g, int nodeId, EvalContext& ctx);

    // Value a node should show in a preview: for sink nodes (no outputs, e.g. Output) the value
    // arriving at input 0, otherwise output pin `pin`. Converted to an image.
    ImagePtr evaluateDisplay(const Graph& g, int nodeId, EvalContext& ctx, int pin = 0);
    Value evaluateOutput(const Graph& g, int nodeId, int pin, EvalContext& ctx);

    // Drop cache entries for nodes that no longer exist.
    void prune(const Graph& g);
    void clearCache() { cache_.clear(); }

    int recomputeCount = 0;  // nodes actually evaluated (for tests / stats)

    // Milliseconds the node's own evaluate() took when it last ran (upstream work excluded), like
    // Blender's compositor Node Timings. -1 if the node has no cached result.
    double nodeMs(int nodeId) const;
    // Timings of every cached node in the graph last evaluated (top level only; a group's time
    // includes its inner nodes).
    std::unordered_map<int, double> timings() const;

private:
    struct Entry {
        size_t sig = 0;
        std::vector<Value> outs;
        double ms = 0;
    };
    size_t ensure(const Graph& g, int nodeId, EvalContext& ctx, std::unordered_map<int, size_t>& pass);

    std::unordered_map<int, Entry> cache_;
};

// Picks the working resolution from the first Image Input node that loads.
void initContextSize(const Graph& g, EvalContext& ctx);

// Runs evaluation on a background thread. Newer submissions replace queued ones; the UI keeps
// showing the last completed result meanwhile. A running job is normally left to finish, so a
// slider drag keeps producing intermediate results; `preempt` cancels it instead, for edits whose
// result is the one that matters (a finished gesture, undo, a typed value).
class AsyncEvaluator {
public:
    struct Result {
        std::vector<ImagePtr> images;    // one per submitted target
        std::vector<std::string> errors;  // one per submitted target (empty = ok)
        double ms = 0;
        uint64_t generation = 0;
        std::unordered_map<int, double> nodeMs;  // top-level node timings (see Evaluator::timings)
    };

    explicit AsyncEvaluator(ImageCache& cache);
    ~AsyncEvaluator();
    AsyncEvaluator(const AsyncEvaluator&) = delete;
    AsyncEvaluator& operator=(const AsyncEvaluator&) = delete;

    // Evaluates several display targets in one pass (they share the node cache).
    // pins[i]: which output of targets[i] to show (defaults to 0).
    void submit(nlohmann::json graphJson, std::vector<NodePath> targets, std::vector<int> pins = {}, bool preempt = false);
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
        uint64_t generation = 0;
    };
    void run();

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
};
