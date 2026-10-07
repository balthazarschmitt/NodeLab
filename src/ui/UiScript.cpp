#include "ui/UiScript.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <fstream>
#include <sstream>

#include <imgui.h>

#include "io/Paths.h"
#include "ui/UiItems.h"

// Reads a double-quoted target. Returns false (leaving the stream) when the next token isn't quoted.
static bool readTarget(std::istringstream& in, std::string& out) {
    in >> std::ws;
    if (in.peek() != '"') return false;
    in.get();
    out.clear();
    for (int c; (c = in.get()) != EOF && c != '"';) out += char(c);
    return true;
}

bool UiScript::load(const std::string& path, std::string& err) {
    std::ifstream f(u8ToPath(path));
    if (!f) {
        err = "cannot open script";
        return false;
    }
    int lineNo = 0;
    auto push = [&](std::string op, float a = 0, float b = 0, std::string s = {}) {
        steps_.push_back({std::move(op), a, b, std::move(s), lineNo});
    };
    // Press, optionally move, release: shared by the numeric and named click/drag forms.
    auto press = [&](float bt) {
        push("wait", 2);
        push("down", bt);
        push("wait", 2);
    };
    auto release = [&](float bt) {
        push("wait", 2);
        push("up", bt);
        push("wait", 2);
    };
    std::string line;
    while (std::getline(f, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream in(line);
        std::string op;
        if (!(in >> op) || op[0] == '#') continue;
        if (op == "wait") {
            float n = 1;
            in >> n;
            push("wait", n);
        } else if (op == "settle" || op == "quit") {
            push(op);
        } else if (std::string t, u; (op == "click" || op == "clickat" || op == "move" || op == "moveat" ||
                                       op == "drag" || op == "dragat" || op == "dragto" || op == "expect") &&
                                      readTarget(in, t)) {
            float fx = 0.5f, fy = 0.5f, dx = 0, dy = 0, bt = 0;
            if (op == "clickat" || op == "moveat" || op == "dragat") in >> fx >> fy;
            if (op == "drag" || op == "dragat") in >> dx >> dy;
            if (op == "dragto" && !readTarget(in, u)) {
                err = "line " + std::to_string(lineNo) + ": dragto needs two quoted targets";
                return false;
            }
            in >> bt;
            if (op == "expect") {
                push("find", 0, 0, t);
                continue;
            }
            push("moveto", fx, fy, t);
            if (op == "move" || op == "moveat") continue;
            press(bt);
            if (op == "drag" || op == "dragat")
                for (int i = 1; i <= 12; ++i) push("moveby", dx * i / 12.0f, dy * i / 12.0f);
            if (op == "dragto")
                for (int i = 1; i <= 12; ++i) push("movetoward", i / 12.0f, 0, u);
            release(bt);
        } else if (op == "move") {
            float x, y;
            in >> x >> y;
            push("move", x, y);
        } else if (op == "wheel") {
            float v = 1;
            in >> v;
            push("wheel", v);
        } else if (op == "down" || op == "up") {
            float bt = 0;
            in >> bt;
            push(op, bt);
        } else if (op == "click") {
            float x, y, bt = 0;
            in >> x >> y >> bt;
            push("move", x, y);
            press(bt);
            push("up", bt);
            push("wait", 2);
        } else if (op == "drag") {
            float x0, y0, x1, y1, bt = 0;
            in >> x0 >> y0 >> x1 >> y1 >> bt;
            push("move", x0, y0);
            press(bt);
            for (int i = 1; i <= 12; ++i) push("move", x0 + (x1 - x0) * i / 12.0f, y0 + (y1 - y0) * i / 12.0f);
            release(bt);
        } else if (op == "text" || op == "shot" || op == "key" || op == "ctrl" || op == "alt" || op == "shift" ||
                   op == "time" || op == "items") {
            std::string rest;
            std::getline(in >> std::ws, rest);
            push(op, 0, 0, rest);
            if (op == "key") push("keyup", 0, 0, rest);
        } else {
            err = "line " + std::to_string(lineNo) + ": unknown command '" + op + "'";
            return false;
        }
    }
    return true;
}

static ImGuiKey keyFromName(const std::string& n) {
    if (n == "enter") return ImGuiKey_Enter;
    if (n == "escape") return ImGuiKey_Escape;
    if (n == "delete") return ImGuiKey_Delete;
    if (n == "backspace") return ImGuiKey_Backspace;
    if (n == "tab") return ImGuiKey_Tab;
    if (n == "slash") return ImGuiKey_Slash;
    if (n == "up") return ImGuiKey_UpArrow;
    if (n == "down") return ImGuiKey_DownArrow;
    if (n == "left") return ImGuiKey_LeftArrow;
    if (n == "right") return ImGuiKey_RightArrow;
    if (n.size() == 1 && n[0] >= '0' && n[0] <= '9') return ImGuiKey(ImGuiKey_0 + (n[0] - '0'));
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return ImGuiKey(ImGuiKey_A + (n[0] - 'a'));
    if (n.size() >= 2 && n[0] == 'f' && std::isdigit(static_cast<unsigned char>(n[1]))) {
        const int k = std::atoi(n.c_str() + 1);
        if (k >= 1 && k <= 12) return ImGuiKey(ImGuiKey_F1 + (k - 1));
    }
    return ImGuiKey_None;
}

std::string UiScript::step(bool evalIdle, bool& quit) {
    if (!active()) {
        quit = true;
        return {};
    }
    if (wait_ > 0) {
        --wait_;
        return {};
    }
    ImGuiIO& io = ImGui::GetIO();
    const Step& s = steps_[pos_];
    std::string shot;
    if (s.op == "settle") {
        settled_ = evalIdle ? settled_ + 1 : 0;
        if (settled_ < 5) return {};
        settled_ = 0;
    } else if (s.op == "wait") {
        wait_ = int(s.a) - 1;
    } else if (s.op == "moveto" || s.op == "movetoward" || s.op == "find") {
        // Named targets come from the last frame; give a window or popup a moment to appear.
        const uiitems::Item* it = uiitems::find(s.s);
        if (!it) {
            if (++searching_ < 200) return {};
            std::fprintf(stderr, "script line %d: no item \"%s\"\n", s.line, s.s.c_str());
            searching_ = 0;
            failed_ = quit = true;
            return {};
        }
        searching_ = 0;
        if (s.op == "moveto") {
            ax_ = it->min.x + (it->max.x - it->min.x) * s.a;
            ay_ = it->min.y + (it->max.y - it->min.y) * s.b;
            io.AddMousePosEvent(ax_, ay_);
        } else if (s.op == "movetoward") {
            const float cx = (it->min.x + it->max.x) * 0.5f, cy = (it->min.y + it->max.y) * 0.5f;
            io.AddMousePosEvent(ax_ + (cx - ax_) * s.a, ay_ + (cy - ay_) * s.a);
        }
    } else if (s.op == "items") {
        for (const uiitems::Item& it : uiitems::all())
            if (s.s.empty() || it.window.find(s.s) != std::string::npos || it.name.find(s.s) != std::string::npos)
                std::fprintf(stderr, "  %s/%s  (%.0f,%.0f)-(%.0f,%.0f)%s%s\n", it.window.c_str(), it.name.c_str(), it.min.x,
                             it.min.y, it.max.x, it.max.y, it.id.empty() ? "" : "  ##", it.id.c_str());
    } else if (s.op == "moveby") {
        io.AddMousePosEvent(ax_ + s.a, ay_ + s.b);
    } else if (s.op == "move") {
        io.AddMousePosEvent(s.a, s.b);
    } else if (s.op == "down" || s.op == "up") {
        io.AddMouseButtonEvent(int(s.a), s.op == "down");
    } else if (s.op == "wheel") {
        io.AddMouseWheelEvent(0.0f, s.a);
    } else if (s.op == "text") {
        io.AddInputCharactersUTF8(s.s.c_str());
    } else if (s.op == "key" || s.op == "keyup") {
        io.AddKeyEvent(keyFromName(s.s), s.op == "key");
    } else if (s.op == "alt" || s.op == "shift") {
        bool on = s.s == "on";
        io.AddKeyEvent(s.op == "alt" ? ImGuiMod_Alt : ImGuiMod_Shift, on);
        io.AddKeyEvent(s.op == "alt" ? ImGuiKey_LeftAlt : ImGuiKey_LeftShift, on);
    } else if (s.op == "ctrl") {
        bool on = s.s == "on";
        io.AddKeyEvent(ImGuiMod_Ctrl, on);
        io.AddKeyEvent(ImGuiKey_LeftCtrl, on);
    } else if (s.op == "shot") {
        shot = s.s;
    } else if (s.op == "time") {
        const auto now = std::chrono::steady_clock::now();
        if (timed_) std::fprintf(stderr, "%s: %.0f ms\n", s.s.c_str(), std::chrono::duration<double, std::milli>(now - *timed_).count());
        timed_ = now;
    } else if (s.op == "quit") {
        quit = true;
    }
    ++pos_;
    return shot;
}
