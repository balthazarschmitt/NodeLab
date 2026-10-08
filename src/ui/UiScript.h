#pragma once
#include <chrono>
#include <optional>
#include <string>
#include <vector>

// Deterministic UI automation for testing: feeds input straight into Dear ImGui (OS mouse/keyboard
// is ignored while a script runs), one step per frame. Script format, one command per line:
//   wait N                 skip N frames
//   settle                 wait until evaluation is idle
//   move X Y               mouse position (framebuffer pixels)
//   down B / up B          mouse button (0 left, 1 right, 2 middle)
//   click X Y [B]          move, press, release
//   drag X0 Y0 X1 Y1 [B]   press at start, move in steps, release at end
// Named targets find an item drawn in the last frame by its label (see UiItems.h). A target is
// "Label" or "Window/Label", in double quotes; "Label#2" is the second match. Icon-only buttons
// answer to their ID ("##Before / After" as "Before / After"), and nodes on the canvas to
// "node:Title" ("node:Title.Field" for a field on its body, "node:Title<Pin" / ">Pin" for pins).
// A missing target waits up to 2 s, then the script fails (exit code 1).
//   click "T" [B]          click the target's centre
//   doubleclick "T"        double-click the target's centre (two quick left clicks)
//   clickat "T" FX FY [B]  click at a fraction of the target's rectangle (0..1 from top left)
//   move "T"               move to the target's centre
//   moveat "T" FX FY       move to a fraction of the target's rectangle
//   drag "T" DX DY [B]     drag from the target's centre by DX, DY pixels
//   dragat "T" FX FY DX DY [B]  drag from a fraction of the target's rectangle by DX, DY
//   dragto "T" "U" [B]     drag from the centre of T to the centre of U
//   expect "T"             fail unless T appears
//   items [TEXT]           print the targets drawn in the last frame (containing TEXT) to stderr
//   text STRING            type characters
//   wheel N                mouse wheel steps (positive = up / zoom in)
//   key NAME               press+release: enter, escape, delete, backspace, tab, slash, up, down, left, right, a-z, 0-9, f1-f12
//   ctrl|alt|shift on|off  hold/release a modifier
//   shot PATH              save the rendered frame as PNG
//   time LABEL             print LABEL and the time since the last `time` to stderr (the first only starts the clock)
//   quit
// Lines starting with '#' are comments.
class UiScript {
public:
    struct Step {
        std::string op;
        float a = 0, b = 0;
        std::string s;
        int line = 0;
    };

    bool load(const std::string& path, std::string& err);
    bool active() const { return pos_ < steps_.size(); }
    bool failed() const { return failed_; }

    // Called once per frame before ImGui::NewFrame(). Returns a screenshot path to save after this
    // frame renders (empty if none); sets quit when the script ends.
    std::string step(bool evalIdle, bool& quit);

private:
    std::vector<Step> steps_;
    size_t pos_ = 0;
    int wait_ = 0;
    int settled_ = 0;
    int searching_ = 0;     // frames spent waiting for a named target
    float ax_ = 0, ay_ = 0;  // where the last named move landed (drags are relative to it)
    bool failed_ = false;
    std::optional<std::chrono::steady_clock::time_point> timed_;
};
