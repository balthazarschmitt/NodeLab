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

    // Preview a node inside the group (path relative to the inner graph), given this group's inputs.
    ImagePtr previewInner(EvalContext& ctx, const std::vector<Value>& inputs, const std::vector<int>& path);

    Graph& inner() { return *inner_; }
    const Graph& inner() const { return *inner_; }

    std::string name = "Group";
    std::vector<PinDesc> ins, outs;  // the group's interface

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
