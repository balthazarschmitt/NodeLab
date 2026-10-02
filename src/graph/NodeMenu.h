#pragma once
// The add-node menu's layout, kept apart from NodeInfo::category (which also picks a node's title
// colour) so the menu can be regrouped without changing how nodes look. Like Blender's Add menu,
// each submenu is split into sections by separators, and no submenu runs much past a screenful.
#include <string>
#include <vector>

namespace nodemenu {

struct Menu {
    std::string name;
    // Node type strings in order; an empty string is a separator.
    std::vector<std::string> items;
};

// Every visible registered node, each in exactly one menu. Types the layout doesn't list (a node
// added without updating it) are appended to the menu named after their category.
const std::vector<Menu>& menus();

// The menu a type is listed in ("" for hidden or unknown types).
const std::string& menuOf(const std::string& type);

}  // namespace nodemenu
