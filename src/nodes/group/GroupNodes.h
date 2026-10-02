#pragma once
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "graph/Graph.h"
#include "nodes/NodeUtil.h"

// Node groups: a GroupNode owns an inner Graph. Inside it, a Group Input node exposes the group's
// input pins as outputs and a Group Output node collects the group's outputs. The pin lists (the
// "interface") live on the GroupNode and are pushed to the inner IO nodes by syncInner().

class GroupInputNode : public Node {
public:
    static const NodeInfo& staticInfo();
    const NodeInfo& info() const override { return info_; }
    void evaluate(EvalContext&, const std::vector<Value>&, std::vector<Value>& out) override;

    void setPins(const std::vector<PinDesc>& pins);
    std::vector<Value> provided;  // values injected by the owning group before evaluation

private:
    NodeInfo info_ = staticInfo();
};

class GroupOutputNode : public Node {
public:
    static const NodeInfo& staticInfo();
    const NodeInfo& info() const override { return info_; }
    void evaluate(EvalContext&, const std::vector<Value>&, std::vector<Value>&) override {}

    void setPins(const std::vector<PinDesc>& pins);

private:
    NodeInfo info_ = staticInfo();
};

class GroupNode : public Node {
public:
    GroupNode();
    static const NodeInfo& staticInfo();
    const NodeInfo& info() const override { return info_; }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;
    void saveExtra(nlohmann::json& j) const override;
    void loadExtra(const nlohmann::json& j) override;
    std::string signatureExtra() const override;
    // Image inputs have no value (their param slot only keeps params aligned with ins).
    bool paramHidden(int i) const override {
        return i < int(ins.size()) && (ins[i].type == PinType::Image || !ranges[size_t(i)].hasValue);
    }

    // Preview a node inside the group (path relative to the inner graph), given this group's inputs.
    ImagePtr previewInner(EvalContext& ctx, const std::vector<Value>& inputs, const std::vector<int>& path, int pin = 0,
                          Value* onGpu = nullptr);

    Graph& inner() { return *inner_; }
    const Graph& inner() const { return *inner_; }

    std::string name = "Group";
    std::vector<PinDesc> ins, outs;  // the group's interface
    // Blender's group socket Default / Min / Max, one per input. Channel and Number inputs show
    // their value (param i) as a slider on the group node while unconnected, so a group can be
    // used like a node with settings.
    struct InputRange {
        float def = 0.0f, min = 0.0f, max = 1.0f;
        // False only for an input from an older file whose inner nodes' sliders disagree: it stays
        // empty while unconnected, so each inner node keeps using its own slider, as before.
        // Setting a range gives it a value.
        bool hasValue = true;
    };
    std::vector<InputRange> ranges;
    // Sets input i's range (min <= max), clamping its default and current value into it.
    void setRange(int i, InputRange r);

    // Rebuilds info_ from name/ins/outs and pushes the interface to the inner IO nodes.
    void syncInner();

    // Interface edits that keep links consistent in both the outer graph and the inner graph.
    void addPin(Graph& outer, bool output, const PinDesc& pin);
    void removePin(Graph& outer, bool output, int index);
    void movePin(Graph& outer, bool output, int index, int dir);
    void setPinType(Graph& outer, bool output, int index, PinType type);

private:
    void injectInputs(const std::vector<Value>& in);

    std::unique_ptr<Graph> inner_;
    NodeInfo info_;
};

// Moves `ids` into a new group node (boundary wires become group pins). Returns its id, or 0.
int groupNodes(Graph& g, const std::set<int>& ids);
// Replaces a group node with its contents. Returns the ids of the restored nodes.
std::vector<int> ungroupNode(Graph& g, int groupId);

// Walks group ids from the root; returns null if the path no longer resolves.
Graph* resolveGroupPath(Graph& root, const std::vector<int>& groupIds);
