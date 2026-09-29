#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "graph/Node.h"

struct Link {
    int id = 0;
    int fromNode = 0, fromPin = 0;  // output side
    int toNode = 0, toPin = 0;      // input side
};

class Graph {
public:
    Node* addNode(const std::string& type, float x = 0, float y = 0);
    void removeNode(int id);
    // New node of the same type and params, offset by (dx, dy). Links are not copied.
    Node* duplicateNode(int id, float dx, float dy);
    Node* find(int id) const;

    // Validates types and cycles; replaces any existing link into (toNode, toPin).
    // Returns the new link id, or 0 if rejected (reason in *why when given).
    int connect(int fromNode, int fromPin, int toNode, int toPin, std::string* why = nullptr);
    void removeLink(int linkId);
    const Link* inputLink(int nodeId, int pin) const;

    // First node of the given type, or 0.
    int firstOfType(const std::string& type) const;
    void clear();

    const std::map<int, std::unique_ptr<Node>>& nodes() const { return nodes_; }
    const std::vector<Link>& links() const { return links_; }

    // Serialization. Path params are written relative to baseDir when given.
    nlohmann::json toJson(const std::filesystem::path* baseDir = nullptr) const;
    // Replaces contents. Throws std::runtime_error on malformed input.
    void fromJson(const nlohmann::json& j, const std::filesystem::path* baseDir = nullptr);

private:
    bool reaches(int from, int target) const;  // is target downstream of from?

    std::map<int, std::unique_ptr<Node>> nodes_;
    std::vector<Link> links_;
    int nextId_ = 1;
};
