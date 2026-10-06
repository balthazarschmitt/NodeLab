#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "graph/Node.h"

class NodeRegistry {
public:
    using Factory = std::function<std::unique_ptr<Node>()>;

    static NodeRegistry& instance();

    template <typename T>
    void add() {
        const NodeInfo& inf = T::staticInfo();
        entries_[inf.type] = Entry{&inf, [] { return std::unique_ptr<Node>(new T()); }};
        order_.push_back(inf.type);
    }

    std::unique_ptr<Node> create(const std::string& type) const;
    const NodeInfo* find(const std::string& type) const;
    // Types in registration order, for building menus.
    const std::vector<std::string>& types() const { return order_; }

private:
    struct Entry {
        const NodeInfo* info;
        Factory factory;
    };
    std::map<std::string, Entry> entries_;
    std::vector<std::string> order_;
};

// Registers every built-in node. Explicit (not static-init) so nothing is dropped by the linker.
void registerAllNodes();

void registerIONodes(NodeRegistry& r);
void registerColorNodes(NodeRegistry& r);
void registerDevelopNodes(NodeRegistry& r);
void registerMathNodes(NodeRegistry& r);
void registerPhotoMergeNodes(NodeRegistry& r);  // math/PhotoMerge.cpp: HDR Merge, Panorama Merge
void registerConverterNodes(NodeRegistry& r);
void registerGroupNodes(NodeRegistry& r);
void registerFilterNodes(NodeRegistry& r);
void registerDenoiseNode(NodeRegistry& r);  // filter/Denoise.cpp, listed among the filters
void registerSpotRemovalNode(NodeRegistry& r);  // filter/SpotRemoval.cpp
void registerSharpeningNodes(NodeRegistry& r);  // filter/Sharpening.cpp: Capture Sharpening, Diffuse or Sharpen
void registerEffectNodes(NodeRegistry& r);      // filter/Effects.cpp: Vignette, Defringe, Border
void registerWatermarkNodes(NodeRegistry& r);   // filter/Watermark.cpp
void registerRemoveNodes(NodeRegistry& r);      // filter/Remove.cpp: content-aware Remove
void registerTransformNodes(NodeRegistry& r);
void registerMatteNodes(NodeRegistry& r);
void registerTextureNodes(NodeRegistry& r);
void registerUtilityNodes(NodeRegistry& r);
