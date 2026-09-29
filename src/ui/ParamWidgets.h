#pragma once
#include <nlohmann/json.hpp>

// Interactive editors for structured params, used in the inspector.

// Tone curves {"master","r","g","b"}: click to add a point, drag to move, right-click to delete.
bool curveEditor(const char* id, nlohmann::json& curves);

// Color ramp {"interp","stops"}: click the bar to add a stop, drag markers to move,
// select a marker to edit its color / position.
bool rampEditor(const char* id, nlohmann::json& ramp);
