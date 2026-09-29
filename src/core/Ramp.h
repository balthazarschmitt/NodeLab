#pragma once
#include <vector>

#include <nlohmann/json.hpp>

// Color ramp (gradient) mapping a 0..1 factor to RGBA through color stops.
struct RampStop {
    float pos = 0;
    float c[4] = {0, 0, 0, 1};
};

struct ColorRamp {
    enum Interp { Linear = 0, Constant = 1, Ease = 2, Smooth = 3 };
    int interp = Linear;
    std::vector<RampStop> stops;  // sorted by pos

    void sort();
    void eval(float t, float out[4]) const;
};

ColorRamp rampFromJson(const nlohmann::json& j, bool sorted = true);
nlohmann::json rampToJson(const ColorRamp& r);
nlohmann::json defaultRamp();  // black -> white
