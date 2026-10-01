#include "graph/Evaluator.h"

#include <chrono>
#include <functional>

#include "io/ImageCache.h"
#include "nodes/group/GroupNodes.h"
#include "nodes/io/IONodes.h"

size_t Evaluator::ensure(const Graph& g, int nodeId, EvalContext& ctx, std::unordered_map<int, size_t>& pass) {
    if (auto it = pass.find(nodeId); it != pass.end()) {
        if (it->second == 0) throw std::runtime_error("graph contains a cycle");
        return it->second;
    }
    pass[nodeId] = 0;  // in progress

    Node* n = g.find(nodeId);
    if (!n) throw std::runtime_error("missing node");
    const NodeInfo& info = n->info();

    std::string key = info.type;
    key += ctx.proxy ? "|p|" : "|f|";
    // Working size and scale: generators (textures) and pixel-sized params depend on them.
    key += std::to_string(ctx.defaultW) + "x" + std::to_string(ctx.defaultH) + "@" + std::to_string(ctx.scale) + "|";
    key += nlohmann::json(n->params).dump();
    key += n->signatureExtra();
    if (n->muted) key += "|muted";
    if (ctx.linear()) key += "|linear";

    std::vector<Value> inputs(info.inputs.size());
    for (size_t i = 0; i < info.inputs.size(); ++i) {
        if (const Link* l = g.inputLink(nodeId, int(i))) {
            size_t up = ensure(g, l->fromNode, ctx, pass);
            key += "|L" + std::to_string(up) + ":" + std::to_string(l->fromPin);
            const auto& outs = cache_[l->fromNode].outs;
            if (l->fromPin < int(outs.size())) inputs[i] = outs[l->fromPin];
            // A wire carrying nothing (e.g. from an image input with no file) acts like no wire.
            if (inputs[i].empty() && info.inputs[i].fallbackParam >= 0)
                inputs[i] = Value(n->paramF(info.inputs[i].fallbackParam));
        } else {
            key += "|-";
            int fp = info.inputs[i].fallbackParam;
            if (fp >= 0) inputs[i] = Value(n->paramF(fp));
        }
    }
    size_t sig = std::hash<std::string>{}(key);
    if (sig == 0) sig = 1;

    Entry& e = cache_[nodeId];
    if (e.sig != sig) {
        if (ctx.cancel && ctx.cancel->load()) throw EvalCancelled();
        std::vector<Value> outs(info.outputs.size());
        const auto t0 = std::chrono::steady_clock::now();
        if (n->muted) {
            // Bypass: each output takes the first connected input of the same type (else any
            // convertible one), like Blender's mute.
            for (size_t o = 0; o < outs.size(); ++o) {
                const PinType t = info.outputs[o].type;
                for (int pass = 0; pass < 2 && outs[o].empty(); ++pass)
                    for (size_t i = 0; i < inputs.size(); ++i) {
                        if (!g.inputLink(nodeId, int(i)) || inputs[i].empty()) continue;
                        bool ok = pass == 0 ? info.inputs[i].type == t : canConvert(info.inputs[i].type, t);
                        if (ok) {
                            outs[o] = inputs[i];
                            break;
                        }
                    }
            }
        } else {
            // Parallel loops inside the node poll this flag between chunks, so a cancel lands
            // mid-node instead of after it.
            parallel::CancelScope scope(ctx.cancel);
            n->evaluate(ctx, inputs, outs);
        }
        ++recomputeCount;
        e.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        e.sig = sig;
        e.outs = std::move(outs);
    }
    pass[nodeId] = sig;
    return sig;
}

Value Evaluator::evaluateOutput(const Graph& g, int nodeId, int pin, EvalContext& ctx) {
    std::unordered_map<int, size_t> pass;
    ensure(g, nodeId, ctx, pass);
    const auto& outs = cache_[nodeId].outs;
    return pin < int(outs.size()) ? outs[pin] : Value();
}

ImagePtr Evaluator::evaluateDisplay(const Graph& g, int nodeId, EvalContext& ctx, int pin) {
    Node* n = g.find(nodeId);
    if (!n) return nullptr;
    Value v;
    if (n->info().outputs.empty()) {
        const Link* l = g.inputLink(nodeId, 0);
        if (!l) return nullptr;
        v = evaluateOutput(g, l->fromNode, l->fromPin, ctx);
    } else {
        v = evaluateOutput(g, nodeId, pin, ctx);
    }
    if (v.empty()) return nullptr;
    return toImage(v, ctx.defaultW, ctx.defaultH);
}

std::vector<Value> Evaluator::gatherInputs(const Graph& g, int nodeId, EvalContext& ctx) {
    Node* n = g.find(nodeId);
    if (!n) return {};
    const NodeInfo& info = n->info();
    std::vector<Value> inputs(info.inputs.size());
    for (size_t i = 0; i < info.inputs.size(); ++i) {
        if (const Link* l = g.inputLink(nodeId, int(i))) inputs[i] = evaluateOutput(g, l->fromNode, l->fromPin, ctx);
        const int fp = info.inputs[i].fallbackParam;
        if (inputs[i].empty() && fp >= 0) inputs[i] = Value(n->paramF(fp));
    }
    return inputs;
}

ImagePtr Evaluator::evaluateDisplayPath(const Graph& g, const NodePath& path, EvalContext& ctx, int pin) {
    if (path.empty()) return nullptr;
    if (path.size() == 1) return evaluateDisplay(g, path[0], ctx, pin);
    auto* group = dynamic_cast<GroupNode*>(g.find(path[0]));
    if (!group) return nullptr;
    std::vector<Value> inputs = gatherInputs(g, path[0], ctx);
    return group->previewInner(ctx, inputs, NodePath(path.begin() + 1, path.end()), pin);
}

double Evaluator::nodeMs(int nodeId) const {
    auto it = cache_.find(nodeId);
    return it == cache_.end() || it->second.sig == 0 ? -1.0 : it->second.ms;
}

std::unordered_map<int, double> Evaluator::timings() const {
    std::unordered_map<int, double> t;
    for (const auto& [id, e] : cache_)
        if (e.sig) t[id] = e.ms;
    return t;
}

void Evaluator::prune(const Graph& g) {
    std::erase_if(cache_, [&](const auto& kv) { return g.find(kv.first) == nullptr; });
}

void initContextSize(const Graph& g, EvalContext& ctx) {
    ctx.colorManagement = g.colorManagement;
    if (!ctx.cache) return;
    for (const auto& [id, n] : g.nodes()) {
        if (n->info().type != ImageInputNode::staticInfo().type) continue;
        // Same decode as the node's, so the file isn't decoded twice. Only the size is needed here,
        // so ask for the (kept) proxy even at full resolution: the full image is cached only while
        // in use, and a RAW would otherwise be decoded once here and again by the node.
        const auto decode = static_cast<const ImageInputNode&>(*n).decode(ctx.linear());
        if (ImagePtr img = ctx.cache->get(n->paramS(0), true, nullptr, decode)) {
            int fw = 0, fh = 0;
            const bool known = ctx.cache->fullSize(n->paramS(0), fw, fh) && fw > 0;
            if (ctx.proxy || !known) {
                ctx.defaultW = img->w;
                ctx.defaultH = img->h;
            } else {
                ctx.defaultW = fw;
                ctx.defaultH = fh;
            }
            ctx.scale = ctx.proxy && known ? float(img->w) / fw : 1.0f;
            return;
        }
    }
}

// ---------------------------------------------------------------- AsyncEvaluator

AsyncEvaluator::AsyncEvaluator(ImageCache& cache) : cache_(cache) {
    thread_ = std::thread([this] { run(); });
}

AsyncEvaluator::~AsyncEvaluator() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
        cancel_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void AsyncEvaluator::submit(nlohmann::json graphJson, std::vector<NodePath> targets, std::vector<int> pins, bool preempt) {
    pins.resize(targets.size(), 0);
    {
        std::lock_guard lock(mutex_);
        // Latest job wins: it replaces any queued (not yet started) job. The running job is left
        // to finish so continuous slider drags still produce steady intermediate results.
        pending_ = Job{std::move(graphJson), std::move(targets), std::move(pins), nextGen_++};
        if (preempt) cancel_ = true;  // reset when the worker picks up the pending job
    }
    cv_.notify_all();
}

void AsyncEvaluator::preempt() {
    std::lock_guard lock(mutex_);
    if (pending_) cancel_ = true;
}

std::optional<AsyncEvaluator::Result> AsyncEvaluator::poll() {
    std::lock_guard lock(mutex_);
    auto r = std::move(done_);
    done_.reset();
    return r;
}

void AsyncEvaluator::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || pending_.has_value(); });
            if (quit_) return;
            job = std::move(*pending_);
            pending_.reset();
            cancel_ = false;
            busy_ = true;
        }

        Result res;
        res.generation = job.generation;
        res.images.resize(job.targets.size());
        res.errors.resize(job.targets.size());
        bool cancelled = false;
        auto t0 = std::chrono::steady_clock::now();
        try {
            Graph g;
            g.fromJson(job.graph);
            EvalContext ctx;
            ctx.proxy = true;
            ctx.cache = &cache_;
            ctx.cancel = &cancel_;
            initContextSize(g, ctx);
            for (size_t t = 0; t < job.targets.size(); ++t) {
                try {
                    res.images[t] = evaluator_.evaluateDisplayPath(g, job.targets[t], ctx, job.pins[t]);
                } catch (const EvalCancelled&) {
                    throw;
                } catch (const std::exception& e) {
                    res.errors[t] = e.what();
                }
            }
            evaluator_.prune(g);
            res.nodeMs = evaluator_.timings();
        } catch (const EvalCancelled&) {
            cancelled = true;
        } catch (const std::exception& e) {
            for (auto& err : res.errors) err = e.what();
        }
        res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        std::lock_guard lock(mutex_);
        busy_ = false;
        if (!cancelled) done_ = std::move(res);
    }
}
