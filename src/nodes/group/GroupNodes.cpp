#include "nodes/group/GroupNodes.h"

#include <algorithm>
#include <map>

#include "graph/Evaluator.h"

namespace {

nlohmann::json pinsToJson(const std::vector<PinDesc>& pins) {
    nlohmann::json a = nlohmann::json::array();
    for (const auto& p : pins) a.push_back({{"name", p.name}, {"type", int(p.type)}});
    return a;
}

std::vector<PinDesc> pinsFromJson(const nlohmann::json& a) {
    std::vector<PinDesc> pins;
    if (!a.is_array()) return pins;
    for (const auto& p : a) {
        int t = std::clamp(p.value("type", 0), 0, 2);
        pins.push_back({p.value("name", std::string("Value")), PinType(t)});
    }
    return pins;
}

}  // namespace

// ---------------------------------------------------------------- group IO nodes

const NodeInfo& GroupInputNode::staticInfo() {
    static const NodeInfo inf{"group.input", "Group Input", "Group", {}, {}, {}, true};
    return inf;
}

void GroupInputNode::setPins(const std::vector<PinDesc>& pins) { info_.outputs = pins; }

void GroupInputNode::evaluate(EvalContext&, const std::vector<Value>&, std::vector<Value>& out) {
    for (size_t i = 0; i < out.size() && i < provided.size(); ++i) out[i] = provided[i];
}

const NodeInfo& GroupOutputNode::staticInfo() {
    static const NodeInfo inf{"group.output", "Group Output", "Group", {}, {}, {}, true};
    return inf;
}

void GroupOutputNode::setPins(const std::vector<PinDesc>& pins) { info_.inputs = pins; }

// ---------------------------------------------------------------- group node

GroupNode::GroupNode() : inner_(std::make_unique<Graph>()) { syncInner(); }

const NodeInfo& GroupNode::staticInfo() {
    // Not in the add menu: groups are made with Ctrl+G from a selection.
    static const NodeInfo inf{"group.group", "Group", "Group", {}, {}, {}, true};
    return inf;
}

void GroupNode::syncInner() {
    info_ = staticInfo();
    info_.displayName = name;
    info_.inputs = ins;
    info_.outputs = outs;
    for (const auto& [id, n] : inner_->nodes()) {
        if (auto* gi = dynamic_cast<GroupInputNode*>(n.get())) gi->setPins(ins);
        if (auto* go = dynamic_cast<GroupOutputNode*>(n.get())) go->setPins(outs);
    }
    // Drop inner links that point at pins which no longer exist.
    for (const auto& [id, n] : inner_->nodes()) {
        if (dynamic_cast<GroupInputNode*>(n.get()))
            inner_->remapPins(id, true, [&](int p) { return p < int(ins.size()) ? p : -1; });
        if (dynamic_cast<GroupOutputNode*>(n.get()))
            inner_->remapPins(id, false, [&](int p) { return p < int(outs.size()) ? p : -1; });
    }
}

void GroupNode::saveExtra(nlohmann::json& j) const {
    j = {{"name", name}, {"inputs", pinsToJson(ins)}, {"outputs", pinsToJson(outs)}, {"graph", inner_->toJson()}};
}

void GroupNode::loadExtra(const nlohmann::json& j) {
    name = j.value("name", std::string("Group"));
    ins = pinsFromJson(j.value("inputs", nlohmann::json::array()));
    outs = pinsFromJson(j.value("outputs", nlohmann::json::array()));
    inner_ = std::make_unique<Graph>();
    if (auto gj = j.find("graph"); gj != j.end()) {
        // IO nodes must know their pins before links to them are restored, so load in two steps.
        nlohmann::json nodesOnly = *gj;
        nlohmann::json links = nodesOnly["links"];
        nodesOnly["links"] = nlohmann::json::array();
        inner_->fromJson(nodesOnly);
        for (const auto& [id, n] : inner_->nodes()) {
            if (auto* gi = dynamic_cast<GroupInputNode*>(n.get())) gi->setPins(ins);
            if (auto* go = dynamic_cast<GroupOutputNode*>(n.get())) go->setPins(outs);
        }
        for (const auto& l : links)
            inner_->connect(l.at("from")[0].get<int>(), l.at("from")[1].get<int>(), l.at("to")[0].get<int>(),
                            l.at("to")[1].get<int>());
    }
    syncInner();
}

std::string GroupNode::signatureExtra() const {
    nlohmann::json j;
    saveExtra(j);
    return j.dump();
}

void GroupNode::injectInputs(const std::vector<Value>& in) {
    for (const auto& [id, n] : inner_->nodes())
        if (auto* gi = dynamic_cast<GroupInputNode*>(n.get())) gi->provided = in;
}

void GroupNode::evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) {
    injectInputs(in);
    // A fresh evaluator per call: the group as a whole is cached by the outer evaluator (its
    // signature includes the inner graph), so inner caching would only help partial re-runs.
    Evaluator ev;
    int outNode = inner_->firstOfType(GroupOutputNode::staticInfo().type);
    if (!outNode) return;
    for (size_t k = 0; k < out.size(); ++k)
        if (const Link* l = inner_->inputLink(outNode, int(k))) out[k] = ev.evaluateOutput(*inner_, l->fromNode, l->fromPin, ctx);
}

ImagePtr GroupNode::previewInner(EvalContext& ctx, const std::vector<Value>& inputs, const std::vector<int>& path) {
    injectInputs(inputs);
    Evaluator ev;
    return ev.evaluateDisplayPath(*inner_, path, ctx);
}

// ---------------------------------------------------------------- interface editing

void GroupNode::addPin(Graph& outer, bool output, const PinDesc& pin) {
    (void)outer;
    (output ? outs : ins).push_back(pin);
    syncInner();
}

void GroupNode::removePin(Graph& outer, bool output, int index) {
    auto& pins = output ? outs : ins;
    if (index < 0 || index >= int(pins.size())) return;
    pins.erase(pins.begin() + index);
    auto shift = [index](int p) { return p == index ? -1 : (p > index ? p - 1 : p); };
    outer.remapPins(id, output, shift);
    for (const auto& [nid, n] : inner_->nodes()) {
        if (!output && dynamic_cast<GroupInputNode*>(n.get())) inner_->remapPins(nid, true, shift);
        if (output && dynamic_cast<GroupOutputNode*>(n.get())) inner_->remapPins(nid, false, shift);
    }
    syncInner();
}

void GroupNode::movePin(Graph& outer, bool output, int index, int dir) {
    auto& pins = output ? outs : ins;
    int other = index + dir;
    if (index < 0 || other < 0 || index >= int(pins.size()) || other >= int(pins.size())) return;
    std::swap(pins[index], pins[other]);
    auto swapIdx = [index, other](int p) { return p == index ? other : (p == other ? index : p); };
    outer.remapPins(id, output, swapIdx);
    for (const auto& [nid, n] : inner_->nodes()) {
        if (!output && dynamic_cast<GroupInputNode*>(n.get())) inner_->remapPins(nid, true, swapIdx);
        if (output && dynamic_cast<GroupOutputNode*>(n.get())) inner_->remapPins(nid, false, swapIdx);
    }
    syncInner();
}

void GroupNode::setPinType(Graph& outer, bool output, int index, PinType type) {
    auto& pins = output ? outs : ins;
    if (index < 0 || index >= int(pins.size())) return;
    pins[index].type = type;
    syncInner();
    // Drop links that became incompatible, on both sides of the boundary.
    auto prune = [](Graph& g) {
        std::vector<int> bad;
        for (const Link& l : g.links()) {
            const Node* a = g.find(l.fromNode);
            const Node* b = g.find(l.toNode);
            if (!a || !b || l.fromPin >= int(a->info().outputs.size()) || l.toPin >= int(b->info().inputs.size()) ||
                !canConvert(a->info().outputs[l.fromPin].type, b->info().inputs[l.toPin].type))
                bad.push_back(l.id);
        }
        for (int id : bad) g.removeLink(id);
    };
    prune(outer);
    prune(*inner_);
}

// ---------------------------------------------------------------- group / ungroup

int groupNodes(Graph& g, const std::set<int>& requested) {
    // The root Output node and group IO nodes can't be moved into a group.
    std::set<int> ids;
    for (int id : requested) {
        const Node* n = g.find(id);
        if (!n) continue;
        const std::string& t = n->info().type;
        if (t == "io.output" || t == GroupInputNode::staticInfo().type || t == GroupOutputNode::staticInfo().type) continue;
        ids.insert(id);
    }
    if (ids.empty()) return 0;

    // Bounds for placement.
    float minX = 1e9f, maxX = -1e9f, sumY = 0, sumX = 0;
    for (int id : ids) {
        const Node* n = g.find(id);
        minX = std::min(minX, n->x);
        maxX = std::max(maxX, n->x);
        sumX += n->x;
        sumY += n->y;
    }
    const float cy = sumY / ids.size(), cx = sumX / ids.size();

    // Inner graph: selected nodes and the links among them, ids preserved.
    nlohmann::json all = g.toJson();
    nlohmann::json innerJ = {{"nextId", all["nextId"]}, {"nodes", nlohmann::json::array()}, {"links", nlohmann::json::array()}};
    for (const auto& n : all["nodes"])
        if (ids.count(n["id"].get<int>())) innerJ["nodes"].push_back(n);
    std::vector<Link> incoming, outgoing;
    for (const Link& l : g.links()) {
        bool fromIn = ids.count(l.fromNode) > 0, toIn = ids.count(l.toNode) > 0;
        if (fromIn && toIn)
            innerJ["links"].push_back({{"id", l.id}, {"from", {l.fromNode, l.fromPin}}, {"to", {l.toNode, l.toPin}}});
        else if (!fromIn && toIn) incoming.push_back(l);
        else if (fromIn && !toIn) outgoing.push_back(l);
    }

    auto* group = dynamic_cast<GroupNode*>(g.addNode(GroupNode::staticInfo().type, cx, cy));
    if (!group) return 0;
    Graph& inner = group->inner();
    inner.fromJson(innerJ);
    auto* gin = dynamic_cast<GroupInputNode*>(inner.addNode(GroupInputNode::staticInfo().type, minX - 260, cy));
    auto* gout = dynamic_cast<GroupOutputNode*>(inner.addNode(GroupOutputNode::staticInfo().type, maxX + 260, cy));

    // One group input per distinct outside source; one group output per distinct inside source.
    std::map<std::pair<int, int>, int> inPinFor, outPinFor;
    for (const Link& l : incoming) {
        auto key = std::make_pair(l.fromNode, l.fromPin);
        if (!inPinFor.count(key)) {
            const PinDesc& target = g.find(l.toNode)->info().inputs[l.toPin];
            inPinFor[key] = int(group->ins.size());
            group->ins.push_back({target.name, target.type});
        }
    }
    for (const Link& l : outgoing) {
        auto key = std::make_pair(l.fromNode, l.fromPin);
        if (!outPinFor.count(key)) {
            const PinDesc& src = g.find(l.fromNode)->info().outputs[l.fromPin];
            outPinFor[key] = int(group->outs.size());
            group->outs.push_back({src.name, src.type});
        }
    }
    group->syncInner();
    for (const Link& l : incoming) inner.connect(gin->id, inPinFor[{l.fromNode, l.fromPin}], l.toNode, l.toPin);
    for (const auto& [key, pin] : outPinFor) inner.connect(key.first, key.second, gout->id, pin);

    for (int id : ids) g.removeNode(id);
    for (const Link& l : incoming) g.connect(l.fromNode, l.fromPin, group->id, inPinFor[{l.fromNode, l.fromPin}]);
    for (const Link& l : outgoing) g.connect(group->id, outPinFor[{l.fromNode, l.fromPin}], l.toNode, l.toPin);
    return group->id;
}

std::vector<int> ungroupNode(Graph& g, int groupId) {
    auto* group = dynamic_cast<GroupNode*>(g.find(groupId));
    if (!group) return {};
    Graph& inner = group->inner();

    // Center the contents where the group node was.
    float sx = 0, sy = 0;
    int count = 0;
    for (const auto& [id, n] : inner.nodes()) {
        if (dynamic_cast<GroupInputNode*>(n.get()) || dynamic_cast<GroupOutputNode*>(n.get())) continue;
        sx += n->x;
        sy += n->y;
        ++count;
    }
    const float dx = count ? group->x - sx / count : 0, dy = count ? group->y - sy / count : 0;

    std::map<int, int> remap;  // inner id -> outer id
    for (const auto& [id, n] : inner.nodes()) {
        if (dynamic_cast<GroupInputNode*>(n.get()) || dynamic_cast<GroupOutputNode*>(n.get())) continue;
        if (Node* c = g.cloneNode(*n, n->x + dx, n->y + dy)) remap[id] = c->id;
    }

    // Outer wires into / out of the group, by pin.
    std::map<int, std::pair<int, int>> sourceForIn;          // group input pin -> outer source
    std::map<int, std::vector<std::pair<int, int>>> targetsForOut;  // group output pin -> outer targets
    for (const Link& l : g.links()) {
        if (l.toNode == groupId) sourceForIn[l.toPin] = {l.fromNode, l.fromPin};
        if (l.fromNode == groupId) targetsForOut[l.fromPin].push_back({l.toNode, l.toPin});
    }

    for (const Link& l : inner.links()) {
        const Node* from = inner.find(l.fromNode);
        const Node* to = inner.find(l.toNode);
        const bool fromGin = dynamic_cast<const GroupInputNode*>(from) != nullptr;
        const bool toGout = dynamic_cast<const GroupOutputNode*>(to) != nullptr;
        if (fromGin && toGout) {
            // pass-through: outer source straight to outer targets
            if (sourceForIn.count(l.fromPin))
                for (auto& t : targetsForOut[l.toPin]) g.connect(sourceForIn[l.fromPin].first, sourceForIn[l.fromPin].second, t.first, t.second);
        } else if (fromGin) {
            if (sourceForIn.count(l.fromPin) && remap.count(l.toNode))
                g.connect(sourceForIn[l.fromPin].first, sourceForIn[l.fromPin].second, remap[l.toNode], l.toPin);
        } else if (toGout) {
            if (remap.count(l.fromNode))
                for (auto& t : targetsForOut[l.toPin]) g.connect(remap[l.fromNode], l.fromPin, t.first, t.second);
        } else if (remap.count(l.fromNode) && remap.count(l.toNode)) {
            g.connect(remap[l.fromNode], l.fromPin, remap[l.toNode], l.toPin);
        }
    }
    g.removeNode(groupId);
    std::vector<int> ids;
    for (auto& [a, b] : remap) ids.push_back(b);
    return ids;
}

Graph* resolveGroupPath(Graph& root, const std::vector<int>& groupIds) {
    Graph* g = &root;
    for (int id : groupIds) {
        auto* group = dynamic_cast<GroupNode*>(g->find(id));
        if (!group) return nullptr;
        g = &group->inner();
    }
    return g;
}

void registerGroupNodes(NodeRegistry& r) {
    r.add<GroupNode>();
    r.add<GroupInputNode>();
    r.add<GroupOutputNode>();
}
