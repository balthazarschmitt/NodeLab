#include "ui/UiScript.h"

#include <fstream>
#include <sstream>

#include <imgui.h>

#include "io/Paths.h"

bool UiScript::load(const std::string& path, std::string& err) {
    std::ifstream f(u8ToPath(path));
    if (!f) {
        err = "cannot open script";
        return false;
    }
    auto push = [&](std::string op, float a = 0, float b = 0, std::string s = {}) {
        steps_.push_back({std::move(op), a, b, std::move(s)});
    };
    std::string line;
    int lineNo = 0;
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
            push("wait", 2);
            push("down", bt);
            push("wait", 2);
            push("up", bt);
            push("wait", 2);
        } else if (op == "drag") {
            float x0, y0, x1, y1, bt = 0;
            in >> x0 >> y0 >> x1 >> y1 >> bt;
            push("move", x0, y0);
            push("wait", 2);
            push("down", bt);
            push("wait", 2);
            for (int i = 1; i <= 12; ++i) push("move", x0 + (x1 - x0) * i / 12.0f, y0 + (y1 - y0) * i / 12.0f);
            push("wait", 2);
            push("up", bt);
            push("wait", 2);
        } else if (op == "text" || op == "shot" || op == "key" || op == "ctrl" || op == "alt" || op == "shift") {
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
    if (n == "up") return ImGuiKey_UpArrow;
    if (n == "down") return ImGuiKey_DownArrow;
    if (n.size() == 1 && n[0] >= 'a' && n[0] <= 'z') return ImGuiKey(ImGuiKey_A + (n[0] - 'a'));
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
    } else if (s.op == "quit") {
        quit = true;
    }
    ++pos_;
    return shot;
}
