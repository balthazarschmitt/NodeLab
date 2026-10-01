#include "graph/Evaluator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>

#ifdef _WIN32
#include <windows.h>  // GlobalMemoryStatusEx, for the cache budget
#endif

#include "io/ImageCache.h"
#include "nodes/group/GroupNodes.h"
#include "nodes/io/IONodes.h"

namespace {
// Thrown when a graph can't be evaluated by region (evaluateRegion then returns null).
struct RoiFailed {};

PixelRect clipRect(PixelRect r, int w, int h) {
    const int x1 = std::min(w, r.x + r.w), y1 = std::min(h, r.y + r.h);
    r.x = std::max(0, r.x);
    r.y = std::max(0, r.y);
    r.w = std::max(0, x1 - r.x);
    r.h = std::max(0, y1 - r.y);
    return r;
}

PixelRect unite(const PixelRect& a, const PixelRect& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    const int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return {x0, y0, x1 - x0, y1 - y0};
}

size_t valueBytes(const Value& v) {
    if (auto p = std::get_if<ImagePtr>(&v.v)) return *p ? (*p)->px.size() * sizeof(float) : 0;
    if (auto p = std::get_if<ChannelPtr>(&v.v)) return *p ? (*p)->data.size() * sizeof(float) : 0;
    return 0;
}

// Nodes upstream of nodeId (inclusive), inputs before the nodes reading them.
void upstreamOrder(const Graph& g, int nodeId, std::vector<int>& order, std::unordered_map<int, int>& state) {
    int& s = state[nodeId];
    if (s == 2) return;
    if (s == 1) throw std::runtime_error("graph contains a cycle");
    s = 1;
    const Node* n = g.find(nodeId);
    if (!n) throw std::runtime_error("missing node");
    for (size_t i = 0; i < n->info().inputs.size(); ++i)
        if (const Link* l = g.inputLink(nodeId, int(i))) upstreamOrder(g, l->fromNode, order, state);
    state[nodeId] = 2;
    order.push_back(nodeId);
}
}  // namespace

std::string Evaluator::baseKey(const Node& n, const EvalContext& ctx) const {
    std::string key = n.info().type;
    key += ctx.proxy ? "|p|" : "|f|";
    // Working size and scale: generators (textures) and pixel-sized params depend on them.
    key += std::to_string(ctx.defaultW) + "x" + std::to_string(ctx.defaultH) + "@" + std::to_string(ctx.scale) + "|";
    key += nlohmann::json(n.params).dump();
    key += n.signatureExtra();
    if (n.muted) key += "|muted";
    if (ctx.linear()) key += "|linear";
    return key;
}

void Evaluator::run(const Graph& g, Node& n, EvalContext& ctx, const std::vector<Value>& inputs, Entry& e, size_t sig) {
    if (ctx.cancel && ctx.cancel->load()) throw EvalCancelled();
    const NodeInfo& info = n.info();
    std::vector<Value> outs(info.outputs.size());
    std::vector<float> stats;
    const auto t0 = std::chrono::steady_clock::now();
    if (n.muted) {
        // Bypass: each output takes the first connected input of the same type (else any
        // convertible one), like Blender's mute.
        for (size_t o = 0; o < outs.size(); ++o) {
            const PinType t = info.outputs[o].type;
            for (int pass = 0; pass < 2 && outs[o].empty(); ++pass)
                for (size_t i = 0; i < inputs.size(); ++i) {
                    if (!g.inputLink(n.id, int(i)) || inputs[i].empty()) continue;
                    bool ok = pass == 0 ? info.inputs[i].type == t : canConvert(info.inputs[i].type, t);
                    if (ok) {
                        outs[o] = inputs[i];
                        break;
                    }
                }
        }
    } else {
        // Whole-image runs record global statistics for regions to reuse.
        std::vector<float>* const savedStats = ctx.statsOut;
        ctx.statsOut = level_ == Region ? nullptr : &stats;
        try {
            // Parallel loops inside the node poll this flag between chunks, so a cancel lands
            // mid-node instead of after it.
            parallel::CancelScope scope(ctx.cancel);
            n.evaluate(ctx, inputs, outs);
        } catch (...) {
            ctx.statsOut = savedStats;
            throw;
        }
        ctx.statsOut = savedStats;
    }
    ++recomputeCount;
    e.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    e.sig = sig;
    e.outs = std::move(outs);
    e.stats = std::move(stats);
}

size_t Evaluator::ensure(const Graph& g, int nodeId, EvalContext& ctx, std::unordered_map<int, size_t>& pass) {
    if (auto it = pass.find(nodeId); it != pass.end()) {
        if (it->second == 0) throw std::runtime_error("graph contains a cycle");
        return it->second;
    }
    pass[nodeId] = 0;  // in progress

    Node* n = g.find(nodeId);
    if (!n) throw std::runtime_error("missing node");
    const NodeInfo& info = n->info();
    auto& cache = cache_[level_];

    std::string key = baseKey(*n, ctx);
    std::vector<Value> inputs(info.inputs.size());
    for (size_t i = 0; i < info.inputs.size(); ++i) {
        if (const Link* l = g.inputLink(nodeId, int(i))) {
            size_t up = ensure(g, l->fromNode, ctx, pass);
            key += "|L" + std::to_string(up) + ":" + std::to_string(l->fromPin);
            const auto& outs = cache[l->fromNode].outs;
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

    Entry& e = cache[nodeId];
    if (e.sig != sig) run(g, *n, ctx, inputs, e, sig);
    if (releaseIntermediates) {
        // This node has read its inputs: drop those no other node still needs.
        inputs.clear();
        for (size_t i = 0; i < info.inputs.size(); ++i)
            if (const Link* l = g.inputLink(nodeId, int(i))) {
                auto it = readers_.find(l->fromNode);
                if (it != readers_.end() && --it->second == 0) {
                    Entry& up = cache[l->fromNode];
                    up.outs.clear();
                    up.outs.shrink_to_fit();
                    up.sig = 0;  // recomputed if asked for again
                }
            }
    }
    pass[nodeId] = sig;
    return sig;
}

Value Evaluator::evaluateOutput(const Graph& g, int nodeId, int pin, EvalContext& ctx) {
    std::unordered_map<int, size_t> pass;
    if (releaseIntermediates) {
        // Count the reads each node's result will get in this pass.
        readers_.clear();
        std::vector<int> order;
        std::unordered_map<int, int> state;
        upstreamOrder(g, nodeId, order, state);
        for (int id : order) {
            const Node* n = g.find(id);
            for (size_t i = 0; i < n->info().inputs.size(); ++i)
                if (const Link* l = g.inputLink(id, int(i))) ++readers_[l->fromNode];
        }
    }
    ensure(g, nodeId, ctx, pass);
    readers_.clear();
    const auto& outs = cache_[level_][nodeId].outs;
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

std::optional<Evaluator::RegionResult> Evaluator::evaluateRegion(const Graph& g, int nodeId, int pin, EvalContext& ctx,
                                                                 float u0, float v0, float u1, float v1) {
    const Node* target = g.find(nodeId);
    if (!target) return std::nullopt;
    if (target->info().outputs.empty()) {  // a sink shows what arrives at its first input
        const Link* l = g.inputLink(nodeId, 0);
        if (!l) return std::nullopt;
        nodeId = l->fromNode;
        pin = l->fromPin;
    }
    struct RestoreLevel {
        Level& l;
        Level v;
        ~RestoreLevel() { l = v; }
    } restore{level_, level_};
    level_ = Region;

    std::vector<int> order;
    std::unordered_map<int, int> state;
    upstreamOrder(g, nodeId, order, state);
    const auto& preview = cache_[Preview];

    // What each node does in the region pass. Sizes are of the full images at the region's scale.
    struct Plan {
        std::vector<bool> sized;    // per output, from the preview
        int w = 0, h = 0;           // full output size (sized nodes)
        PixelRect req;              // part of the output its consumers need
        PixelRect win;              // part the node runs on (req grown by its padding)
        std::vector<PixelRect> in;  // part of each sized input it reads
        int inW = 0, inH = 0;       // full size of its sized inputs
        bool anySized() const { return std::find(sized.begin(), sized.end(), true) != sized.end(); }
    };
    std::unordered_map<int, Plan> plan;
    auto inputSized = [&](const Link* l) {
        const Plan& up = plan[l->fromNode];
        return l->fromPin < int(up.sized.size()) && up.sized[l->fromPin];
    };

    try {
        // Sizes, from the sources down. The preview tells which values carry pixels.
        for (int id : order) {
            const Node& n = *g.find(id);
            const NodeInfo& info = n.info();
            auto it = preview.find(id);
            if (it == preview.end() || it->second.sig == 0) throw RoiFailed{};
            Plan& p = plan[id];
            p.sized.resize(info.outputs.size());
            int pw, ph;
            for (size_t o = 0; o < info.outputs.size(); ++o)
                p.sized[o] = o < it->second.outs.size() && it->second.outs[o].size(pw, ph);
            p.in.resize(info.inputs.size());
            bool anyIn = false;
            int inW = 0, inH = 0;
            for (size_t i = 0; i < info.inputs.size(); ++i) {
                const Link* l = g.inputLink(id, int(i));
                if (!l || !inputSized(l)) continue;
                const Plan& up = plan[l->fromNode];
                // Inputs of different sizes are stretched onto each other; regions can't follow that.
                if (anyIn && (up.w != inW || up.h != inH)) throw RoiFailed{};
                anyIn = true;
                inW = up.w;
                inH = up.h;
            }
            if (!p.anySized()) {
                // Numbers made from images (Image Info, statistics) depend on the whole image.
                if (anyIn) throw RoiFailed{};
                continue;
            }
            if (n.roiSourceSize(ctx, p.w, p.h)) continue;
            if (!anyIn) {
                p.w = ctx.defaultW;
                p.h = ctx.defaultH;
            } else if (n.muted) {
                p.w = inW;
                p.h = inH;
            } else {
                n.roiOutputSize(inW, inH, p.w, p.h);
            }
        }

        // Requests, from the target up.
        Plan& tp = plan[nodeId];
        if (pin >= int(tp.sized.size()) || !tp.sized[pin]) return std::nullopt;
        {
            const int x0 = std::clamp(int(std::floor(u0 * tp.w)), 0, tp.w);
            const int y0 = std::clamp(int(std::floor(v0 * tp.h)), 0, tp.h);
            const int x1 = std::clamp(int(std::ceil(u1 * tp.w)), 0, tp.w);
            const int y1 = std::clamp(int(std::ceil(v1 * tp.h)), 0, tp.h);
            tp.req = {x0, y0, x1 - x0, y1 - y0};
            if (tp.req.empty()) return std::nullopt;
        }
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const int id = *it;
            Plan& p = plan[id];
            if (p.req.empty()) continue;  // sizeless, or not needed for the target's region
            const Node& n = *g.find(id);
            const NodeInfo& info = n.info();
            int& inW = p.inW;
            int& inH = p.inH;
            for (size_t i = 0; i < info.inputs.size(); ++i)
                if (const Link* l = g.inputLink(id, int(i)); l && inputSized(l)) {
                    inW = plan[l->fromNode].w;
                    inH = plan[l->fromNode].h;
                }
            PixelRect inRect;
            int sw, sh;
            // The node's policy sees its full size through ctx.roi (nodeutil::frameOf).
            RoiWindow asked{p.req, p.w, p.h};
            EvalContext pctx = ctx;
            pctx.roi = &asked;
            const std::vector<float>& stats = preview.at(id).stats;
            pctx.previewStats = stats.empty() ? nullptr : &stats;
            if (n.roiSourceSize(ctx, sw, sh)) {
                p.win = p.req;
            } else if (!n.muted && inW > 0 && n.roiMap(pctx, inW, inH, p.req, inRect)) {
                p.win = p.req;
                inRect = clipRect(inRect, inW, inH);
            } else {
                int pad = n.muted ? 0 : n.roiPadding(pctx);
                // A node changing the size without mapping regions can only run whole.
                if (inW > 0 && (inW != p.w || inH != p.h)) pad = Node::kRoiWhole;
                if (pad == Node::kRoiWhole) {
                    // Running it whole is fine when the region is most of the image anyway;
                    // otherwise the viewer keeps the preview rather than render the full image.
                    if (int64_t(p.w) * p.h > 4 * int64_t(p.req.w) * p.req.h) throw RoiFailed{};
                    p.win = {0, 0, p.w, p.h};
                    inRect = {0, 0, inW, inH};
                } else {
                    p.win = clipRect({p.req.x - pad, p.req.y - pad, p.req.w + 2 * pad, p.req.h + 2 * pad}, p.w, p.h);
                    inRect = p.win;
                }
            }
            for (size_t i = 0; i < info.inputs.size(); ++i)
                if (const Link* l = g.inputLink(id, int(i)); l && inputSized(l)) {
                    p.in[i] = inRect;
                    plan[l->fromNode].req = unite(plan[l->fromNode].req, inRect);
                }
        }

        // Evaluate, from the sources down, each node on its window.
        auto& cache = cache_[Region];
        std::unordered_map<int, size_t> sigs;
        for (int id : order) {
            Node& n = *g.find(id);
            const NodeInfo& info = n.info();
            Plan& p = plan[id];
            if (p.req.empty()) {
                // Sized nodes outside the target's region aren't needed at all.
                if (p.anySized()) continue;
                // Sizeless and fed by no images (values, constants): the preview's result holds.
                Entry& e = cache[id];
                const Entry& pe = preview.at(id);
                if (e.sig != pe.sig) {
                    e.sig = pe.sig;
                    e.outs = pe.outs;
                }
                sigs[id] = e.sig;
                continue;
            }
            std::string key = baseKey(n, ctx);
            for (const PixelRect& r : {p.win, p.req, PixelRect{0, 0, p.w, p.h}})
                key += "|R" + std::to_string(r.x) + "," + std::to_string(r.y) + "," + std::to_string(r.w) + "," +
                       std::to_string(r.h);
            std::vector<Value> inputs(info.inputs.size());
            for (size_t i = 0; i < info.inputs.size(); ++i) {
                if (const Link* l = g.inputLink(id, int(i))) {
                    key += "|L" + std::to_string(sigs.at(l->fromNode)) + ":" + std::to_string(l->fromPin);
                    const Entry& up = cache.at(l->fromNode);
                    if (l->fromPin < int(up.outs.size())) inputs[i] = up.outs[l->fromPin];
                    if (inputSized(l)) {
                        const PixelRect& ur = plan[l->fromNode].req;
                        const PixelRect& r = p.in[i];
                        inputs[i] = cropValue(inputs[i], r.x - ur.x, r.y - ur.y, r.w, r.h);
                    }
                    if (inputs[i].empty() && info.inputs[i].fallbackParam >= 0)
                        inputs[i] = Value(n.paramF(info.inputs[i].fallbackParam));
                } else {
                    key += "|-";
                    const int fp = info.inputs[i].fallbackParam;
                    if (fp >= 0) inputs[i] = Value(n.paramF(fp));
                }
            }
            size_t sig = std::hash<std::string>{}(key);
            if (sig == 0) sig = 1;
            Entry& e = cache[id];
            if (e.sig != sig) {
                RoiWindow window{p.win, p.w, p.h};
                for (const PixelRect& r : p.in)
                    if (!r.empty()) window.input = r;
                window.inputW = p.inW;
                window.inputH = p.inH;
                const std::vector<float>& stats = preview.at(id).stats;
                EvalContext rctx = ctx;
                rctx.roi = &window;
                rctx.statsOut = nullptr;
                rctx.previewStats = stats.empty() ? nullptr : &stats;
                run(g, n, rctx, inputs, e, sig);
                // Every sized output must cover the window; keep the requested part.
                for (Value& v : e.outs) {
                    int vw, vh;
                    if (!v.size(vw, vh)) continue;
                    if (vw != p.win.w || vh != p.win.h) {
                        e.sig = 0;
                        throw RoiFailed{};
                    }
                    v = cropValue(v, p.req.x - p.win.x, p.req.y - p.win.y, p.req.w, p.req.h);
                }
            }
            sigs[id] = sig;
        }
        // Only this region's nodes are kept.
        std::erase_if(cache, [&](const auto& kv) { return !sigs.count(kv.first); });

        const auto& outs = cache.at(nodeId).outs;
        RegionResult r;
        r.image = pin < int(outs.size()) ? toImage(outs[pin], tp.req.w, tp.req.h) : nullptr;
        if (!r.image) return std::nullopt;
        r.u0 = float(tp.req.x) / tp.w;
        r.v0 = float(tp.req.y) / tp.h;
        r.u1 = float(tp.req.x + tp.req.w) / tp.w;
        r.v1 = float(tp.req.y + tp.req.h) / tp.h;
        return r;
    } catch (const RoiFailed&) {
        return std::nullopt;
    }
}

double Evaluator::nodeMs(int nodeId) const {
    const auto& cache = cache_[level_];
    auto it = cache.find(nodeId);
    return it == cache.end() || it->second.sig == 0 ? -1.0 : it->second.ms;
}

std::unordered_map<int, double> Evaluator::timings() const {
    std::unordered_map<int, double> t;
    for (const auto& [id, e] : cache_[level_])
        if (e.sig) t[id] = e.ms;
    return t;
}

void Evaluator::prune(const Graph& g) {
    for (auto& cache : cache_) std::erase_if(cache, [&](const auto& kv) { return g.find(kv.first) == nullptr; });
}

size_t Evaluator::bytes(Level l) const {
    size_t b = 0;
    for (const auto& [id, e] : cache_[l])
        for (const Value& v : e.outs) b += valueBytes(v);
    return b;
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
        if (ImagePtr img = ctx.cache->get(n->paramS(0), true, nullptr, decode, ctx.proxyEdge)) {
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

bool initRegionContext(const Graph& g, EvalContext& ctx, float scale) {
    ctx.colorManagement = g.colorManagement;
    ctx.proxy = false;
    ctx.scale = scale;
    if (!ctx.cache) return false;
    for (const auto& [id, n] : g.nodes()) {
        if (n->info().type != ImageInputNode::staticInfo().type) continue;
        int fw = 0, fh = 0;
        if (!ctx.cache->fullSize(n->paramS(0), fw, fh)) continue;
        ImageCache::levelSize(fw, fh, scale, ctx.defaultW, ctx.defaultH);
        return true;
    }
    return false;
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

void AsyncEvaluator::submit(nlohmann::json graphJson, std::vector<NodePath> targets, std::vector<int> pins, bool preempt,
                            Options options) {
    pins.resize(targets.size(), 0);
    {
        std::lock_guard lock(mutex_);
        // Latest job wins: it replaces any queued (not yet started) job. The running job is left
        // to finish so continuous slider drags still produce steady intermediate results.
        pending_ = Job{std::move(graphJson), std::move(targets), std::move(pins), std::move(options), nextGen_++};
        // Reset when the worker picks up the pending job. Details of an outdated graph or view
        // aren't worth finishing.
        if (preempt || inDetails_) cancel_ = true;
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
        res.draft = job.options.draft;
        res.images.resize(job.targets.size());
        res.errors.resize(job.targets.size());
        bool cancelled = false;
        auto t0 = std::chrono::steady_clock::now();
        Graph g;
        EvalContext ctx;
        try {
            g.fromJson(job.graph);
            ctx.proxy = true;
            ctx.cache = &cache_;
            ctx.cancel = &cancel_;
            ctx.proxyEdge = std::max(64, job.options.draft ? job.options.proxyEdge / 2 : job.options.proxyEdge);
            evaluator_.setLevel(job.options.draft ? Evaluator::Draft : Evaluator::Preview);
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

        const bool details = !cancelled && !job.options.draft && !job.options.details.empty();
        bool inDetails;
        {
            std::lock_guard lock(mutex_);
            if (!cancelled) done_ = std::move(res);
            // A newer job already waiting makes the details pointless.
            inDetails = inDetails_ = details && !pending_;
            if (!inDetails) busy_ = false;
        }
        if (!inDetails) {
            trimCache();
            continue;
        }

        // The preview is on screen; now the sharper parts of zoomed-in views.
        std::vector<Tile> tiles;
        bool detailsDone = true;
        try {
            tiles = evaluateDetails(g, job, ctx);
        } catch (const EvalCancelled&) {
            detailsDone = false;
        } catch (const std::exception&) {
            tiles.clear();  // keep the preview
        }
        trimCache();
        std::lock_guard lock(mutex_);
        inDetails_ = false;
        busy_ = false;
        if (!detailsDone) continue;
        if (done_ && done_->generation == job.generation) {
            done_->tiles = std::move(tiles);
            done_->tilesDone = true;
        } else {
            Result r;
            r.generation = job.generation;
            r.tiles = std::move(tiles);
            r.tilesDone = true;
            done_ = std::move(r);
        }
    }
}

std::vector<AsyncEvaluator::Tile> AsyncEvaluator::evaluateDetails(const Graph& g, const Job& job, EvalContext& ctx) {
    std::vector<Tile> tiles;
    for (size_t i = 0; i < job.options.details.size(); ++i) {
        const Detail& d = job.options.details[i];
        Tile t;
        t.tag = d.tag;
        tiles.push_back(t);
        // How many full-resolution pixels the view shows per screen pixel decides the level:
        // the smallest of 1, 1/2, 1/4... that is at least as sharp as the screen, like a mipmap.
        ImagePtr preview = evaluator_.evaluateDisplay(g, d.node, ctx, d.pin);
        if (!preview || preview->w <= 0 || d.screenW <= 0) continue;
        const float need = ctx.scale * d.screenW / float(preview->w);
        if (need <= ctx.scale * 1.15f) continue;  // the preview is (nearly) as sharp
        float level = 1.0f;
        while (level * 0.5f >= need) level *= 0.5f;
        if (level <= ctx.scale) continue;
        EvalContext rctx;
        rctx.cache = &cache_;
        rctx.cancel = &cancel_;
        if (!initRegionContext(g, rctx, level)) continue;
        // A margin around the visible part, so a little panning stays sharp.
        const float mu = (d.u1 - d.u0) * 0.1f, mv = (d.v1 - d.v0) * 0.1f;
        auto r = evaluator_.evaluateRegion(g, d.node, d.pin, rctx, std::max(0.0f, d.u0 - mu), std::max(0.0f, d.v0 - mv),
                                           std::min(1.0f, d.u1 + mu), std::min(1.0f, d.v1 + mv));
        if (!r || !r->image) continue;
        Tile& out = tiles.back();
        out.image = r->image;
        out.u0 = r->u0, out.v0 = r->v0, out.u1 = r->u1, out.v1 = r->v1;
    }
    return tiles;
}

namespace {
// Memory the evaluation caches may hold before levels other than the preview are dropped: a
// quarter of the RAM, roughly darktable's default resource level.
size_t cacheBudget() {
    static const size_t budget = [] {
        size_t ram = size_t(8) << 30;
#ifdef _WIN32
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) ram = size_t(ms.ullTotalPhys);
#endif
        return std::max<size_t>(ram / 4, size_t(512) << 20);
    }();
    return budget;
}
}  // namespace

void AsyncEvaluator::trimCache() {
    // The preview is what every edit re-evaluates incrementally, so it is kept; the draft and the
    // regions are cheap to recompute by comparison (the region cache only helps edits that keep
    // the view where it is).
    const size_t budget = cacheBudget();
    size_t total = evaluator_.bytes(Evaluator::Preview);
    for (Evaluator::Level l : {Evaluator::Region, Evaluator::Draft}) {
        const size_t b = evaluator_.bytes(l);
        if (total + b > budget) evaluator_.clearLevel(l);
        else total += b;
    }
}
