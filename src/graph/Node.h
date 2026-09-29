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

enum class ParamKind { Float, Int, Bool, Enum, Path, Text, Curve, Ramp, Color, SavePath };

struct ParamDesc {
    std::string name;
    ParamKind kind = ParamKind::Float;
    float min = 0.0f, max = 1.0f;          // slider (soft) range
    float hardMin = 0.0f, hardMax = 1.0f;  // values are clamped to this
    nlohmann::json def;
    std::vector<std::string> options;  // Enum labels

    // Clamped to [mn, mx].
    static ParamDesc Float(std::string n, float def, float mn, float mx) { return make(std::move(n), ParamKind::Float, mn, mx, mn, mx, def); }
    // Slider shows [mn, mx] but any value is allowed (math inputs).
    static ParamDesc FloatFree(std::string n, float def, float mn, float mx) {
        return make(std::move(n), ParamKind::Float, mn, mx, -1e6f, 1e6f, def);
    }
    static ParamDesc Int(std::string n, int def, int mn, int mx) {
        return make(std::move(n), ParamKind::Int, float(mn), float(mx), float(mn), float(mx), def);
    }
    static ParamDesc Bool(std::string n, bool def) { return make(std::move(n), ParamKind::Bool, 0, 1, 0, 1, def); }
    static ParamDesc Enum(std::string n, int def, std::vector<std::string> opts) {
        float mx = float(opts.size()) - 1;
        ParamDesc d = make(std::move(n), ParamKind::Enum, 0, mx, 0, mx, def);
        d.options = std::move(opts);
        return d;
    }
    static ParamDesc Path(std::string n) { return make(std::move(n), ParamKind::Path, 0, 0, 0, 0, ""); }
    static ParamDesc Text(std::string n, std::string def) { return make(std::move(n), ParamKind::Text, 0, 0, 0, 0, std::move(def)); }
    // Tone curves: {"master":[[x,y],...], "r":[...], "g":[...], "b":[...]}.
    static ParamDesc Curve(std::string n);
    // Curves with custom channels. keys: "key:Label" entries (add "@hue" for a hue-strip
    // background); def: {"key": [[x,y],...], ...}.
    static ParamDesc CurveKeys(std::string n, std::vector<std::string> keys, nlohmann::json def) {
        ParamDesc d = make(std::move(n), ParamKind::Curve, 0, 0, 0, 0, std::move(def));
        d.options = std::move(keys);
        return d;
    }
    // RGB color stored as [r, g, b]; range is the allowed component range.
    static ParamDesc Color(std::string n, float r, float g, float b, float mx = 1.0f) {
        return make(std::move(n), ParamKind::Color, 0, mx, 0, mx, nlohmann::json::array({r, g, b}));
    }
    // File path chosen with a Save dialog (File Output node).
    static ParamDesc SavePath(std::string n) { return make(std::move(n), ParamKind::SavePath, 0, 0, 0, 0, ""); }
    // Color ramp: {"interp": 0..3, "stops": [[pos, r, g, b, a], ...]}.
    static ParamDesc Ramp(std::string n);

private:
    static ParamDesc make(std::string n, ParamKind k, float mn, float mx, float hmn, float hmx, nlohmann::json def) {
        ParamDesc d;
        d.name = std::move(n);
        d.kind = k;
        d.min = mn;
        d.max = mx;
        d.hardMin = hmn;
        d.hardMax = hmx;
        d.def = std::move(def);
        return d;
    }
};

struct NodeInfo {
    std::string type;         // stable id used in save files, e.g. "color.saturation"
    std::string displayName;  // shown in the UI
    std::string category;     // add-menu grouping
    std::vector<PinDesc> inputs;
    std::vector<PinDesc> outputs;
    std::vector<ParamDesc> params;
    bool hidden = false;  // not offered in the add-node menu (group internals)
};

struct EvalContext {
    int defaultW = 512, defaultH = 512;  // size used when a node has no sized inputs
    bool proxy = true;                   // preview-resolution sources
    // Preview pixels per full-resolution pixel. Sizes in params (blur radius, offsets) are in
    // full-resolution pixels and multiplied by this, so previews match the exported image.
    float scale = 1.0f;
    ImageCache* cache = nullptr;
    const std::atomic<bool>* cancel = nullptr;
};

class Node {
public:
    virtual ~Node() = default;
    virtual const NodeInfo& info() const = 0;
    // inputs[i] is already filled with the fallback param value for unconnected pins that have one.
    virtual void evaluate(EvalContext& ctx, const std::vector<Value>& inputs, std::vector<Value>& outputs) = 0;

    // Extra per-node state beyond params (node groups store their inner graph here).
    virtual void saveExtra(nlohmann::json&) const {}
    virtual void loadExtra(const nlohmann::json&) {}
    // Folded into the evaluation cache key so changes to extra state trigger recomputation.
    virtual std::string signatureExtra() const { return {}; }

    void initParams() {
        params.clear();
        for (const auto& p : info().params) params.push_back(p.def);
    }

    float paramF(int i) const { return params[i].is_number() ? params[i].get<float>() : 0.0f; }
    int paramI(int i) const { return params[i].is_number() ? params[i].get<int>() : 0; }
    bool paramB(int i) const { return params[i].is_boolean() ? params[i].get<bool>() : false; }
    std::string paramS(int i) const { return params[i].is_string() ? params[i].get<std::string>() : std::string(); }
    void paramC(int i, float out[3]) const {
        for (int k = 0; k < 3; ++k)
            out[k] = params[i].is_array() && params[i].size() == 3 && params[i][k].is_number() ? params[i][k].get<float>() : 0.0f;
    }

    int id = 0;
    float x = 0.0f, y = 0.0f;  // editor grid position
    std::vector<nlohmann::json> params;
};
