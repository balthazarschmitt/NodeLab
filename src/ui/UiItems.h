#pragma once
#include <string>
#include <vector>

#include <imgui.h>

// Names and rectangles of the items drawn in the last frame, so UI scripts can click a control by
// its label instead of by position. ImGui widgets report themselves through the test-engine hooks
// (IMGUI_ENABLE_TEST_ENGINE, without the test engine library); hand-drawn items such as the node
// editor's nodes call uiitems::add. Recording is off unless a script turns it on.
namespace uiitems {

struct Item {
    std::string window;  // root window's visible name ("Inspector")
    std::string name;    // visible label, or the ID part after "##" when the label is only an icon
    std::string id;      // the part after "##"/"###", if any
    ImVec2 min, max;     // clipped to the window, in screen pixels
};

void setRecording(bool on);
bool recording();

// Reports a hand-drawn item in the current window.
void add(const char* label, const ImVec2& min, const ImVec2& max);

// Swaps the frame being recorded with the finished one. Call once per frame before NewFrame().
void endFrame();

// Finds an item in the last finished frame. `target` is "Label" or "Window/Label"; a trailing
// "#N" picks the Nth match (1-based). Widgets that report no label (combos, colour buttons, a
// window's close button "#CLOSE") match the label they were created with, if it was created
// directly in the window or under the same PushID. Returns nullptr when nothing matches.
const Item* find(const std::string& target);

// The last finished frame's items, for the script's `items` command.
const std::vector<Item>& all();

}  // namespace uiitems
