#pragma once
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
//   text STRING            type characters
//   key NAME               press+release: enter, escape, delete, backspace, tab
//   ctrl on|off            hold/release Ctrl
//   shot PATH              save the rendered frame as PNG
//   quit
// Lines starting with '#' are comments.
class UiScript {
public:
    struct Step {
        std::string op;
        float a = 0, b = 0;
        std::string s;
    };

    bool load(const std::string& path, std::string& err);
    bool active() const { return pos_ < steps_.size(); }

    // Called once per frame before ImGui::NewFrame(). Returns a screenshot path to save after this
    // frame renders (empty if none); sets quit when the script ends.
    std::string step(bool evalIdle, bool& quit);

private:
    std::vector<Step> steps_;
    size_t pos_ = 0;
    int wait_ = 0;
    int settled_ = 0;
};
