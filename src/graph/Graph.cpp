#include "graph/Graph.h"

#include <algorithm>
#include <stdexcept>

#include "graph/NodeRegistry.h"
#include "io/Paths.h"

namespace fs = std::filesystem;

Node* Graph::addNode(const std::string& type, float x, float y) {
    auto node = NodeRegistry::instance().create(type);
    if (!node) return nullptr;
    node->id = nextId_++;
    node->x = x;
    node->y = y;
    Node* raw = node.get();
    nodes_[raw->id] = std::move(node);
    return raw;
}

Node* Graph::duplicateNode(int id, float dx, float dy) {
    Node* src = find(id);
    return src ? cloneNode(*src, src->x + dx, src->y + dy) : nullptr;
}

Node* Graph::cloneNode(const Node& src, float x, float y) {
    // Snapshot first: src may be in this graph, and adding a node must not disturb it.
    nlohmann::json extra;
    src.saveExtra(extra);
    auto params = src.params;
    Node* n = addNode(src.info().type, x, y);
    if (n) {
        n->params = std::move(params);
        if (!extra.is_null()) n->loadExtra(extra);
    }
    return n;
}

void Graph::remapPins(int nodeId, bool outputs, const std::function<int(int)>& map) {
    std::vector<Link> kept;
    for (Link l : links_) {
        if (outputs && l.fromNode == nodeId) {
            l.fromPin = map(l.fromPin);
            if (l.fromPin < 0) continue;
        } else if (!outputs && l.toNode == nodeId) {
            l.toPin = map(l.toPin);
            if (l.toPin < 0) continue;
        }
        kept.push_back(l);
    }
    links_ = std::move(kept);
}

void Graph::removeNode(int id) {
    nodes_.erase(id);
    std::erase_if(links_, [id](const Link& l) { return l.fromNode == id || l.toNode == id; });
}

Node* Graph::find(int id) const {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : it->second.get();
}

bool Graph::reaches(int from, int target) const {
    if (from == target) return true;
    std::vector<int> stack{from};
    std::vector<int> seen;
    while (!stack.empty()) {
        int n = stack.back();
        stack.pop_back();
        if (n == target) return true;
        if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
        seen.push_back(n);
        for (const auto& l : links_)
            if (l.fromNode == n) stack.push_back(l.toNode);
    }
    return false;
}

int Graph::connect(int fromNode, int fromPin, int toNode, int toPin, std::string* why) {
    auto fail = [&](const char* msg) {
        if (why) *why = msg;
        return 0;
    };
    Node* a = find(fromNode);
    Node* b = find(toNode);
    if (!a || !b) return fail("unknown node");
    const auto& outs = a->info().outputs;
    const auto& ins = b->info().inputs;
    if (fromPin < 0 || fromPin >= int(outs.size()) || toPin < 0 || toPin >= int(ins.size()))
        return fail("unknown pin");
    if (!canConvert(outs[fromPin].type, ins[toPin].type))
        return fail("incompatible pin types");
    if (reaches(toNode, fromNode)) return fail("would create a cycle");

    std::erase_if(links_, [&](const Link& l) { return l.toNode == toNode && l.toPin == toPin; });
    Link l{nextId_++, fromNode, fromPin, toNode, toPin};
    links_.push_back(l);
    return l.id;
}

int Graph::pruneInvalidLinks() {
    size_t before = links_.size();
    std::erase_if(links_, [&](const Link& l) {
        const Node* a = find(l.fromNode);
        const Node* b = find(l.toNode);
        return !a || !b || l.fromPin < 0 || l.toPin < 0 || l.fromPin >= int(a->info().outputs.size()) ||
               l.toPin >= int(b->info().inputs.size());
    });
    return int(before - links_.size());
}

void Graph::removeLink(int linkId) {
    std::erase_if(links_, [linkId](const Link& l) { return l.id == linkId; });
}

const Link* Graph::inputLink(int nodeId, int pin) const {
    for (const auto& l : links_)
        if (l.toNode == nodeId && l.toPin == pin) return &l;
    return nullptr;
}

int Graph::firstOfType(const std::string& type) const {
    for (const auto& [id, n] : nodes_)
        if (n->info().type == type) return id;
    return 0;
}

Frame* Graph::addFrame(float x, float y, float w, float h) {
    Frame f;
    f.id = nextId_++;
    f.x = x;
    f.y = y;
    f.w = w;
    f.h = h;
    frames_.push_back(f);
    return &frames_.back();
}

void Graph::removeFrame(int id) {
    std::erase_if(frames_, [id](const Frame& f) { return f.id == id; });
}

Frame* Graph::findFrame(int id) {
    for (auto& f : frames_)
        if (f.id == id) return &f;
    return nullptr;
}

void Graph::clear() {
    nodes_.clear();
    links_.clear();
    frames_.clear();
    nextId_ = 1;
}

nlohmann::json Graph::toJson(const fs::path* baseDir) const {
    nlohmann::json j;
    j["nextId"] = nextId_;
    auto& jn = j["nodes"] = nlohmann::json::array();
    for (const auto& [id, n] : nodes_) {
        nlohmann::json o;
        o["id"] = id;
        o["type"] = n->info().type;
        o["pos"] = {n->x, n->y};
        auto& jp = o["params"] = nlohmann::json::object();
        const auto& descs = n->info().params;
        for (size_t i = 0; i < descs.size(); ++i) {
            nlohmann::json v = n->params[i];
            const bool isPath = descs[i].kind == ParamKind::Path || descs[i].kind == ParamKind::SavePath;
            if (isPath && baseDir && v.is_string() && !v.get<std::string>().empty())
                v = makeRelativeU8(v.get<std::string>(), *baseDir);
            jp[descs[i].name] = v;
        }
        nlohmann::json extra;
        n->saveExtra(extra);
        if (!extra.is_null()) o["extra"] = extra;
        jn.push_back(o);
    }
    auto& jl = j["links"] = nlohmann::json::array();
    for (const auto& l : links_)
        jl.push_back({{"id", l.id}, {"from", {l.fromNode, l.fromPin}}, {"to", {l.toNode, l.toPin}}});
    if (!frames_.empty()) {
        auto& jf = j["frames"] = nlohmann::json::array();
        for (const auto& f : frames_)
            jf.push_back({{"id", f.id}, {"label", f.label}, {"rect", {f.x, f.y, f.w, f.h}},
                          {"color", {f.color[0], f.color[1], f.color[2]}}});
    }
    return j;
}

void Graph::fromJson(const nlohmann::json& j, const fs::path* baseDir) {
    clear();
    int maxId = 0;
    for (const auto& o : j.at("nodes")) {
        std::string type = o.at("type").get<std::string>();
        auto node = NodeRegistry::instance().create(type);
        if (!node) throw std::runtime_error("unknown node type: " + type);
        node->id = o.at("id").get<int>();
        if (auto p = o.find("pos"); p != o.end() && p->is_array() && p->size() == 2) {
            node->x = (*p)[0].get<float>();
            node->y = (*p)[1].get<float>();
        }
        const auto& descs = node->info().params;
        if (auto jp = o.find("params"); jp != o.end()) {
            for (size_t i = 0; i < descs.size(); ++i) {
                auto v = jp->find(descs[i].name);
                if (v == jp->end()) continue;  // keep default for params added after the file was written
                nlohmann::json val = *v;
                const bool isPath = descs[i].kind == ParamKind::Path || descs[i].kind == ParamKind::SavePath;
                if (isPath && baseDir && val.is_string() && !val.get<std::string>().empty())
                    val = makeAbsoluteU8(val.get<std::string>(), *baseDir);
                node->params[i] = val;
            }
        }
        if (auto ex = o.find("extra"); ex != o.end()) node->loadExtra(*ex);
        maxId = std::max(maxId, node->id);
        nodes_[node->id] = std::move(node);
    }
    for (const auto& o : j.at("links")) {
        Link l;
        l.id = o.at("id").get<int>();
        l.fromNode = o.at("from")[0].get<int>();
        l.fromPin = o.at("from")[1].get<int>();
        l.toNode = o.at("to")[0].get<int>();
        l.toPin = o.at("to")[1].get<int>();
        if (!find(l.fromNode) || !find(l.toNode)) continue;
        maxId = std::max(maxId, l.id);
        links_.push_back(l);
    }
    if (auto jf = j.find("frames"); jf != j.end() && jf->is_array()) {
        for (const auto& o : *jf) {
            Frame f;
            f.id = o.value("id", 0);
            f.label = o.value("label", std::string("Frame"));
            if (auto r = o.find("rect"); r != o.end() && r->size() == 4) {
                f.x = (*r)[0].get<float>();
                f.y = (*r)[1].get<float>();
                f.w = (*r)[2].get<float>();
                f.h = (*r)[3].get<float>();
            }
            if (auto c = o.find("color"); c != o.end() && c->size() == 3)
                for (int k = 0; k < 3; ++k) f.color[k] = (*c)[k].get<float>();
            maxId = std::max(maxId, f.id);
            frames_.push_back(f);
        }
    }
    nextId_ = std::max(j.value("nextId", 1), maxId + 1);
}
