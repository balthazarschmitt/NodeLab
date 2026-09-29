#include "graph/NodeRegistry.h"

NodeRegistry& NodeRegistry::instance() {
    static NodeRegistry r;
    return r;
}

std::unique_ptr<Node> NodeRegistry::create(const std::string& type) const {
    auto it = entries_.find(type);
    if (it == entries_.end()) return nullptr;
    auto node = it->second.factory();
    node->initParams();
    return node;
}

const NodeInfo* NodeRegistry::find(const std::string& type) const {
    auto it = entries_.find(type);
    return it == entries_.end() ? nullptr : it->second.info;
}

void registerAllNodes() {
    static bool done = false;
    if (done) return;
    done = true;
    auto& r = NodeRegistry::instance();
    registerIONodes(r);
    registerColorNodes(r);
    registerMathNodes(r);
    registerConverterNodes(r);
    registerGroupNodes(r);
}
