#include "core/Ramp.h"

#include <algorithm>
#include <cmath>

#include "graph/Node.h"

void ColorRamp::sort() {
    std::stable_sort(stops.begin(), stops.end(), [](const RampStop& a, const RampStop& b) { return a.pos < b.pos; });
}

void ColorRamp::eval(float t, float out[4]) const {
    if (stops.empty()) {
        out[0] = out[1] = out[2] = t;
        out[3] = 1;
        return;
    }
    const RampStop* lo = &stops.front();
    const RampStop* hi = &stops.back();
    if (t <= lo->pos) {
        std::copy(lo->c, lo->c + 4, out);
        return;
    }
    if (t >= hi->pos) {
        std::copy(hi->c, hi->c + 4, out);
        return;
    }
    for (size_t i = 0; i + 1 < stops.size(); ++i)
        if (t >= stops[i].pos && t <= stops[i + 1].pos) {
            lo = &stops[i];
            hi = &stops[i + 1];
            break;
        }
    float f = (t - lo->pos) / std::max(hi->pos - lo->pos, 1e-6f);
    switch (interp) {
        case Constant: f = 0.0f; break;  // hold each stop's color until the next stop
        case Ease: f = f * f * (3.0f - 2.0f * f); break;
        case Smooth: f = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f); break;
        default: break;
    }
    for (int k = 0; k < 4; ++k) out[k] = lo->c[k] + (hi->c[k] - lo->c[k]) * f;
}

ColorRamp rampFromJson(const nlohmann::json& j, bool sorted) {
    ColorRamp r;
    if (j.is_object()) {
        r.interp = std::clamp(j.value("interp", 0), 0, 3);
        if (auto s = j.find("stops"); s != j.end() && s->is_array())
            for (const auto& e : *s) {
                if (!e.is_array() || e.size() < 5) continue;
                RampStop st;
                st.pos = std::clamp(e[0].get<float>(), 0.0f, 1.0f);
                for (int k = 0; k < 4; ++k) st.c[k] = std::clamp(e[k + 1].get<float>(), 0.0f, 1.0f);
                r.stops.push_back(st);
            }
    }
    if (r.stops.empty()) {
        r.stops = {RampStop{0.0f, {0, 0, 0, 1}}, RampStop{1.0f, {1, 1, 1, 1}}};
    }
    if (sorted) r.sort();
    return r;
}

nlohmann::json rampToJson(const ColorRamp& r) {
    nlohmann::json s = nlohmann::json::array();
    for (const auto& st : r.stops) s.push_back({st.pos, st.c[0], st.c[1], st.c[2], st.c[3]});
    return {{"interp", r.interp}, {"stops", s}};
}

nlohmann::json defaultRamp() { return rampToJson(rampFromJson(nlohmann::json())); }

ParamDesc ParamDesc::Ramp(std::string n) { return make(std::move(n), ParamKind::Ramp, 0, 0, 0, 0, defaultRamp()); }
