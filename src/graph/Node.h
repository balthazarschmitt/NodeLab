#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Value.h"

class ImageCache;

struct PinDesc {
    std::string name;
    PinType type;
    // For inputs: index of a param whose value is used when the pin is unconnected
    // (so a slider can be replaced by a wire, e.g. a channel driving "Amount" per pixel).
    int fallbackParam = -1;
};

enum class ParamKind { Float, Int, Bool, Enum, Path };

struct ParamDesc {
    std::string name;
    ParamKind kind = ParamKind::Float;
    float min = 0.0f, max = 1.0f;
    nlohmann::json def;
    std::vector<std::string> options;  // Enum labels

    static ParamDesc Float(std::string n, float def, float mn, float mx) {
        return {std::move(n), ParamKind::Float, mn, mx, def, {}};
    }
    static ParamDesc Int(std::string n, int def, int mn, int mx) {
        return {std::move(n), ParamKind::Int, float(mn), float(mx), def, {}};
    }
    static ParamDesc Bool(std::string n, bool def) { return {std::move(n), ParamKind::Bool, 0, 1, def, {}}; }
    static ParamDesc Enum(std::string n, int def, std::vector<std::string> opts) {
        return {std::move(n), ParamKind::Enum, 0, float(opts.size() - 1), def, std::move(opts)};
    }
    static ParamDesc Path(std::string n) { return {std::move(n), ParamKind::Path, 0, 0, "", {}}; }
};

struct NodeInfo {
    std::string type;         // stable id used in save files, e.g. "color.saturation"
    std::string displayName;  // shown in the UI
    std::string category;     // add-menu grouping
    std::vector<PinDesc> inputs;
    std::vector<PinDesc> outputs;
    std::vector<ParamDesc> params;
};

struct EvalContext {
    int defaultW = 512, defaultH = 512;  // size used when a node has no sized inputs
    bool proxy = true;                   // preview-resolution sources
    ImageCache* cache = nullptr;
    const std::atomic<bool>* cancel = nullptr;
};

class Node {
public:
    virtual ~Node() = default;
    virtual const NodeInfo& info() const = 0;
    // inputs[i] is already filled with the fallback param value for unconnected pins that have one.
    virtual void evaluate(EvalContext& ctx, const std::vector<Value>& inputs, std::vector<Value>& outputs) = 0;

    void initParams() {
        params.clear();
        for (const auto& p : info().params) params.push_back(p.def);
    }

    float paramF(int i) const { return params[i].is_number() ? params[i].get<float>() : 0.0f; }
    int paramI(int i) const { return params[i].is_number() ? params[i].get<int>() : 0; }
    bool paramB(int i) const { return params[i].is_boolean() ? params[i].get<bool>() : false; }
    std::string paramS(int i) const { return params[i].is_string() ? params[i].get<std::string>() : std::string(); }

    int id = 0;
    float x = 0.0f, y = 0.0f;  // editor grid position
    std::vector<nlohmann::json> params;
};
