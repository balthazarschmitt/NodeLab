#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// Interactive editors for structured params, used in the inspector.

// Tone curves {"master","r","g","b"}: click to add a point, drag to move, right-click to delete.
// keys: "key:Label" channel list (empty = master/R/G/B); "@hue" draws a hue strip behind the curve.
bool curveEditor(const char* id, nlohmann::json& curves, const std::vector<std::string>& keys = {});

// Color ramp {"interp","stops"}: click the bar to add a stop, drag markers to move,
// select a marker to edit its color / position.
bool rampEditor(const char* id, nlohmann::json& ramp);
