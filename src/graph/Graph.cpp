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
    if (!src) return nullptr;
    Node* n = addNode(src->info().type, src->x + dx, src->y + dy);
    if (n) {
        n->params = src->params;
        nlohmann::json extra;
        src->saveExtra(extra);
        if (!extra.is_null()) n->loadExtra(extra);
    }
    return n;
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

void Graph::clear() {
    nodes_.clear();
    links_.clear();
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
            if (descs[i].kind == ParamKind::Path && baseDir && v.is_string() && !v.get<std::string>().empty())
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
                if (descs[i].kind == ParamKind::Path && baseDir && val.is_string() && !val.get<std::string>().empty())
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
    nextId_ = std::max(j.value("nextId", 1), maxId + 1);
}
