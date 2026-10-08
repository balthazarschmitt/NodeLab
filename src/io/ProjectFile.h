#pragma once
#include <string>

#include <nlohmann/json.hpp>

#include "graph/Graph.h"

constexpr const char* kProjectExt = ".refract";
constexpr const char* kLegacyProjectExt = ".nlproj";  // NodeLab's projects (before 1.7)

// The "app" value of a project file: Refractory's, or NodeLab's from before the rename.
inline bool isProjectApp(const std::string& app) { return app == "Refractory" || app == "NodeLab"; }

// A .refract or .nlproj path (any case).
bool isProjectPath(const std::string& pathU8);

// .refract format: {"app":"Refractory","version":2,"appVersion":"0.3.0","graph":{...},"ui":{...}}
// "version" is the file format; bump it only for changes older builds can't read, and teach
// loadProject to upgrade the old layout. "appVersion" records which Refractory saved the file.
// Image paths are stored relative to the project file's folder.
// Before 1.7 the app was called NodeLab: its projects say "app":"NodeLab" and end in .nlproj, and
// they load as they are.
// Version 2 (0.7): scene-linear projects (graph "colorManagement" block). Older builds would load
// them without linearising images, so they're refused; legacy projects still save as version 1.
constexpr int kProjectVersion = 2;

bool saveProject(const std::string& pathU8, const Graph& g, const nlohmann::json& ui, std::string& err);
bool loadProject(const std::string& pathU8, Graph& g, nlohmann::json& ui, std::string& err);
