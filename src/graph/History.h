#pragma once
#include <string>

#include <nlohmann/json.hpp>

// Names for the History panel's steps (Lightroom's and darktable's history): what changed between
// two graph snapshots (Graph::toJson), such as "Add Curves", "Basic: Exposure 0.50",
// "Delete 3 nodes" or "Connect". Changes inside a group are named after the group.
std::string describeChange(const nlohmann::json& before, const nlohmann::json& after);
