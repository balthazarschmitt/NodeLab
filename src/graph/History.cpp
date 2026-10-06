#include "graph/History.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "graph/NodeRegistry.h"

namespace {

using json = nlohmann::json;

// Nodes by id, so snapshots compare node by node whatever their order.
std::map<int, const json*> nodesById(const json& g) {
    std::map<int, const json*> out;
    if (const auto n = g.find("nodes"); n != g.end() && n->is_array())
        for (const json& o : *n)
            if (o.is_object() && o.contains("id") && o["id"].is_number_integer()) out[o["id"].get<int>()] = &o;
    return out;
}

const NodeInfo* infoOf(const json& node) {
    const auto t = node.find("type");
    return t != node.end() && t->is_string() ? NodeRegistry::instance().find(t->get<std::string>()) : nullptr;
}

// As Node::title: the label, a group's name, or the type's display name.
std::string titleOf(const json& node) {
    if (const auto l = node.find("label"); l != node.end() && l->is_string() && !l->get<std::string>().empty())
        return l->get<std::string>();
    if (const auto e = node.find("extra"); e != node.end() && e->is_object())
        if (const auto n = e->find("name"); n != e->end() && n->is_string() && e->contains("graph")) return n->get<std::string>();
    const NodeInfo* info = infoOf(node);
    return info ? info->displayName : std::string("Node");
}

std::string valueText(const json& node, const std::string& param, const json& v) {
    const NodeInfo* info = infoOf(node);
    const ParamDesc* d = nullptr;
    if (info)
        for (const ParamDesc& p : info->params)
            if (p.name == param) d = &p;
    if (d && d->kind == ParamKind::Enum && v.is_number()) {
        const int i = v.get<int>();
        return i >= 0 && i < int(d->options.size()) ? d->options[size_t(i)] : std::string();
    }
    if (v.is_boolean()) return v.get<bool>() ? "On" : "Off";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number()) {
        char buf[32];
        const double x = v.get<double>();
        std::snprintf(buf, sizeof buf, std::abs(x) >= 100 ? "%.0f" : "%.2f", x);
        return buf;
    }
    return {};  // curves, ramps, colours, text: the name alone says enough
}

json field(const json& o, const char* key) {
    const auto it = o.find(key);
    return it == o.end() ? json() : *it;
}

// What changed on one node that is in both snapshots, or "".
std::string describeNode(const json& a, const json& b) {
    const std::string title = titleOf(b);
    const json pa = field(a, "params"), pb = field(b, "params");
    std::vector<std::string> changed;
    if (pb.is_object())
        for (const auto& [k, v] : pb.items())
            if (!pa.is_object() || !pa.contains(k) || pa[k] != v) changed.push_back(k);
    if (changed.size() == 1) {
        const std::string v = valueText(b, changed[0], pb[changed[0]]);
        // "Gamma 1.85", not "Gamma: Gamma 1.85".
        return (changed[0] == title ? "" : title + ": ") + changed[0] + (v.empty() ? "" : " " + v);
    }
    if (changed.size() > 1) return title + ": " + std::to_string(changed.size()) + " settings";
    const json ea = field(a, "extra"), eb = field(b, "extra");
    if (ea != eb) {
        // A group: name what changed inside it.
        if (ea.is_object() && eb.is_object() && ea.contains("graph") && eb.contains("graph") && ea["graph"] != eb["graph"])
            return title + ": " + describeChange(ea["graph"], eb["graph"]);
        if (ea.is_object() && eb.is_object() && field(ea, "name") != field(eb, "name")) return "Rename " + title;
        return "Edit " + title;
    }
    if (field(a, "muted") != field(b, "muted")) return (field(b, "muted").is_null() ? "Unmute " : "Mute ") + title;
    if (field(a, "label") != field(b, "label")) return "Rename " + title;
    if (field(a, "collapsed") != field(b, "collapsed")) return (field(b, "collapsed").is_null() ? "Expand " : "Collapse ") + title;
    return {};
}

std::set<std::string> linkSet(const json& g) {
    std::set<std::string> out;
    if (const auto l = g.find("links"); l != g.end() && l->is_array())
        for (const json& o : *l) out.insert(field(o, "from").dump() + ">" + field(o, "to").dump());
    return out;
}

}  // namespace

std::string describeChange(const json& before, const json& after) {
    const auto na = nodesById(before), nb = nodesById(after);
    std::vector<const json*> added, removed;
    for (const auto& [id, n] : nb)
        if (!na.count(id)) added.push_back(n);
    for (const auto& [id, n] : na)
        if (!nb.count(id)) removed.push_back(n);
    if (!added.empty() && removed.empty())
        return "Add " + (added.size() == 1 ? titleOf(*added[0]) : std::to_string(added.size()) + " nodes");
    if (!removed.empty() && added.empty())
        return "Delete " + (removed.size() == 1 ? titleOf(*removed[0]) : std::to_string(removed.size()) + " nodes");
    if (!added.empty()) return "Replace nodes";

    std::vector<std::string> edits;
    size_t moved = 0;
    std::string movedTitle;
    for (const auto& [id, b] : nb) {
        const json& a = *na.at(id);
        if (std::string d = describeNode(a, *b); !d.empty()) edits.push_back(std::move(d));
        else if (field(a, "pos") != field(*b, "pos")) ++moved, movedTitle = titleOf(*b);
    }
    if (edits.size() == 1) return edits[0];
    if (edits.size() > 1) return "Edit " + std::to_string(edits.size()) + " nodes";

    const auto la = linkSet(before), lb = linkSet(after);
    if (la != lb) {
        bool gained = false, lost = false;
        for (const auto& l : lb) gained |= !la.count(l);
        for (const auto& l : la) lost |= !lb.count(l);
        return gained && lost ? "Reconnect" : gained ? "Connect" : "Disconnect";
    }
    if (field(before, "colorManagement") != field(after, "colorManagement")) return "Color Management";
    if (field(before, "frames") != field(after, "frames")) return "Edit frames";
    if (moved == 1) return "Move " + movedTitle;
    if (moved > 1) return "Move " + std::to_string(moved) + " nodes";
    return "Edit";
}
