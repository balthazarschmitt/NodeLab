#pragma once
#include "graph/Node.h"

class Graph;

// Layer Stack (Mix): Photoshop's layers as one node. Any number of image inputs, Layer 0 at the
// bottom, each drawn over the ones below with its own Opacity (a slider, or a mask wired to its
// pin) and blend mode. The pins are per instance: connecting the top layer adds an empty one
// above it, and the Inspector adds and removes layers. The layer count and each layer's settings
// are kept in "extra", because params load before the node knows how many layers it has.
class LayerStackNode : public Node {
public:
    static constexpr const char* kType = "math.layer_stack";
    static constexpr int kMinLayers = 2, kMaxLayers = 32;
    // Per layer: pins Image (2i) and Opacity (2i + 1); params Opacity (2i) and Mode (2i + 1).
    static constexpr int kStride = 2;

    LayerStackNode();
    static const NodeInfo& staticInfo();
    const NodeInfo& info() const override { return info_; }
    void evaluate(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;
    bool gpuSupported(const EvalContext&, const std::vector<Value>& in) const override;
    void evaluateGpu(EvalContext& ctx, const std::vector<Value>& in, std::vector<Value>& out) override;
    void saveExtra(nlohmann::json& j) const override;
    void loadExtra(const nlohmann::json& j) override;
    void linksChanged(Graph& g) override;
    void resetParams() override;

    int layers() const { return int(info_.inputs.size()) / kStride; }
    // Sets the number of layers, keeping the settings of those that stay (no link changes).
    void setLayers(int n);
    // Inspector edits that keep the graph's links on the right layers.
    void addLayer(Graph& g);
    void removeLayer(Graph& g, int layer);
    void moveLayer(Graph& g, int layer, int dir);
    // Whether a layer has anything wired into it (its image or its opacity).
    bool layerUsed(const Graph& g, int layer) const;

private:
    NodeInfo info_;
};
