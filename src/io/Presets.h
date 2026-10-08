#pragma once
// Node presets: a selection of nodes (often one group with its sliders) saved under a name, to
// insert into any project from the Add menu's Presets submenu. Each is a JSON file in the user's
// presets folder (%APPDATA%\Refractory\presets\<name>.rfpreset), holding the nodes and the wires
// between them in the clipboard's format, so a preset inserts exactly as a paste does.
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace presets {

// The folder presets live in (created on demand). Tests point it elsewhere with setFolder.
std::string folder();
void setFolder(const std::string& dirU8);

// Preset names, sorted case-insensitively.
std::vector<std::string> list();
// clip: {"nodes": [...], "links": [...]} as copied. Overwrites a preset of the same name.
// Names are trimmed; characters a file name can't hold are replaced. False (with err) on failure.
bool save(const std::string& name, const nlohmann::json& clip, std::string& err);
// The preset's nodes and links, or null if it is missing or damaged.
nlohmann::json load(const std::string& name);
bool remove(const std::string& name);

}  // namespace presets
