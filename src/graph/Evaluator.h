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

#include "graph/Graph.h"

struct EvalCancelled : std::runtime_error {
    EvalCancelled() : std::runtime_error("cancelled") {}
};

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

private:
    struct Entry {
        size_t sig = 0;
        std::vector<Value> outs;
    };
    size_t ensure(const Graph& g, int nodeId, EvalContext& ctx, std::unordered_map<int, size_t>& pass);

    std::unordered_map<int, Entry> cache_;
};

// Picks the working resolution from the first Image Input node that loads.
void initContextSize(const Graph& g, EvalContext& ctx);

// Runs evaluation on a background thread. Newer submissions replace queued ones; the UI keeps
// showing the last completed result meanwhile. Cancellation is used only on shutdown.
class AsyncEvaluator {
public:
    struct Result {
        std::vector<ImagePtr> images;    // one per submitted target
        std::vector<std::string> errors;  // one per submitted target (empty = ok)
        double ms = 0;
        uint64_t generation = 0;
    };

    explicit AsyncEvaluator(ImageCache& cache);
    ~AsyncEvaluator();
    AsyncEvaluator(const AsyncEvaluator&) = delete;
    AsyncEvaluator& operator=(const AsyncEvaluator&) = delete;

    // Evaluates several display targets in one pass (they share the node cache).
    // pins[i]: which output of targets[i] to show (defaults to 0).
    void submit(nlohmann::json graphJson, std::vector<NodePath> targets, std::vector<int> pins = {});
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
