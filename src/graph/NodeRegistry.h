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
void registerMathNodes(NodeRegistry& r);
void registerConverterNodes(NodeRegistry& r);
