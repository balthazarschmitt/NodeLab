#pragma once
#include <filesystem>
#include <functional>
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

// A labelled box drawn behind nodes for organisation. Purely visual: evaluation ignores it.
struct Frame {
    int id = 0;
    std::string label = "Frame";
    float x = 0, y = 0, w = 320, h = 220;  // grid units
    float color[3] = {0.30f, 0.34f, 0.42f};
};

class Graph {
public:
    Node* addNode(const std::string& type, float x = 0, float y = 0);
    void removeNode(int id);
    // New node of the same type and params, offset by (dx, dy). Links are not copied.
    Node* duplicateNode(int id, float dx, float dy);
    // Copy of a node that may live in another graph (type, params, extra state), at (x, y).
    Node* cloneNode(const Node& src, float x, float y);
    // Replaces a node with one of another type in place (Blender's Swap), keeping its id, position,
    // mute/collapse state, params that share a name and kind, and links whose pins can be matched
    // by name, then by position, then by the first compatible free pin. Returns the new node, or
    // null for an unknown type.
    Node* swapNode(int id, const std::string& type);
    // Re-indexes pins on links touching `nodeId` (outputs or inputs). map(old) returns the new
    // index, or -1 to drop the link.
    void remapPins(int nodeId, bool outputs, const std::function<int(int)>& map);
    Node* find(int id) const;

    // Validates types and cycles; replaces any existing link into (toNode, toPin).
    // Returns the new link id, or 0 if rejected (reason in *why when given).
    int connect(int fromNode, int fromPin, int toNode, int toPin, std::string* why = nullptr);
    void removeLink(int linkId);
    void removeLinksOf(int nodeId);
    // Reconnects the node's inputs straight to whatever its outputs fed (Blender's dissolve /
    // Ctrl+X). The node's own links are removed; the node itself stays.
    void bridgeNode(int nodeId);
    // Drops links whose nodes or pin indices no longer exist. Returns how many were removed.
    int pruneInvalidLinks();
    const Link* inputLink(int nodeId, int pin) const;

    Frame* addFrame(float x, float y, float w, float h);
    void removeFrame(int id);
    Frame* findFrame(int id);
    const std::vector<Frame>& frames() const { return frames_; }

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
    std::vector<Frame> frames_;
    int nextId_ = 1;
};
