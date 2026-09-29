#pragma once
#include <string>

#include <nlohmann/json.hpp>

#include "graph/Graph.h"

// .nlproj format: {"app":"NodeLab","version":1,"appVersion":"0.3.0","graph":{...},"ui":{...}}
// "version" is the file format; bump it only for changes older builds can't read, and teach
// loadProject to upgrade the old layout. "appVersion" records which NodeLab saved the file.
// Image paths are stored relative to the project file's folder.
constexpr int kProjectVersion = 1;

bool saveProject(const std::string& pathU8, const Graph& g, const nlohmann::json& ui, std::string& err);
bool loadProject(const std::string& pathU8, Graph& g, nlohmann::json& ui, std::string& err);
