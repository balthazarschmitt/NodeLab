#include "ui/UiItems.h"

#include <cstdlib>

#include <imgui_internal.h>

namespace uiitems {
namespace {

bool on = false;
std::vector<Item> current, finished;

// Every item with an ID, named or not: widgets that don't report a label (combos, colour buttons,
// close buttons) are found by hashing the target with the ID seed they were submitted under,
// as ImGui's test engine does.
struct Anon {
    std::string window;
    ImGuiID id = 0, seed = 0;
    ImVec2 min, max;
};
std::vector<Anon> anonCurrent, anonFinished;
const Item* fromAnon(const Anon& a) {
    static Item it;
    it = Item{a.window, {}, {}, a.min, a.max};
    return &it;
}

std::string trim(std::string s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    size_t b = 0;
    while (b < s.size() && s[b] == ' ') ++b;
    return s.substr(b);
}

// Drops icon-font glyphs (Unicode private use area, U+E000..U+F8FF), so "<icon> Export" is "Export".
std::string dropIcons(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c == 0xEE || c == 0xEF) && i + 2 < s.size()) {
            const unsigned cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                                (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            if (cp >= 0xE000 && cp <= 0xF8FF) {
                i += 3;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// Splits "Visible##id" / "Visible###id" into its visible part and its id part.
void split(const char* label, std::string& visible, std::string& id) {
    const std::string s = label ? label : "";
    const size_t h = s.find("##");
    visible = trim(dropIcons(s.substr(0, h)));
    id.clear();
    if (h != std::string::npos) {
        size_t b = h;
        while (b < s.size() && s[b] == '#') ++b;
        id = s.substr(b);
    }
}

std::string windowName(ImGuiWindow* w) {
    if (!w) return {};
    std::string visible, id;
    split(w->RootWindow ? w->RootWindow->Name : w->Name, visible, id);
    return visible;
}

void record(ImGuiWindow* w, const char* label, ImRect bb) {
    if (!w) return;
    bb.ClipWith(w->ClipRect);
    if (bb.GetWidth() <= 0 || bb.GetHeight() <= 0) return;
    Item it;
    split(label, it.name, it.id);
    if (it.name.empty()) it.name = it.id;
    if (it.name.empty()) return;
    it.window = windowName(w);
    it.min = bb.Min;
    it.max = bb.Max;
    current.push_back(std::move(it));
}

}  // namespace

void setRecording(bool v) {
    on = v;
    if (ImGuiContext* g = ImGui::GetCurrentContext()) g->TestEngineHookItems = v;
}

bool recording() { return on; }

void add(const char* label, const ImVec2& min, const ImVec2& max) {
    if (on) record(ImGui::GetCurrentWindow(), label, ImRect(min, max));
}

void endFrame() {
    finished.swap(current);
    current.clear();
    anonFinished.swap(anonCurrent);
    anonCurrent.clear();
}

const std::vector<Item>& all() { return finished; }

const Item* find(const std::string& target) {
    std::string t = target;
    int nth = 1;
    if (const size_t h = t.rfind('#'); h != std::string::npos && h + 1 < t.size() && h > 0 && t[h - 1] != '#') {
        const char* digits = t.c_str() + h + 1;
        char* end = nullptr;
        const long n = std::strtol(digits, &end, 10);
        if (end && *end == 0 && n > 0) {
            nth = int(n);
            t.resize(h);
        }
    }
    auto scan = [&](const std::string* window, const std::string& name) -> const Item* {
        int seen = 0;
        for (const Item& it : finished)
            if ((it.name == name || it.id == name) && (!window || it.window == *window) && ++seen == nth) return &it;
        // Unlabelled widgets: the name is the label they were created with ("Theme", "##curve").
        seen = 0;
        for (const Anon& a : anonFinished) {
            if (window && a.window != *window) continue;
            if ((ImHashStr(name.c_str(), 0, a.seed) == a.id || ImHashStr(("##" + name).c_str(), 0, a.seed) == a.id) && ++seen == nth)
                return fromAnon(a);
        }
        return nullptr;
    };
    // A label may itself contain '/' ("Before / After"), so try the whole string first.
    if (const Item* it = scan(nullptr, t)) return it;
    for (size_t s = t.find('/'); s != std::string::npos; s = t.find('/', s + 1)) {
        const std::string window = trim(t.substr(0, s));
        if (const Item* it = scan(&window, trim(t.substr(s + 1)))) return it;
    }
    return nullptr;
}

}  // namespace uiitems

// ImGui's test-engine hooks (IMGUI_ENABLE_TEST_ENGINE). ItemInfo carries the label (the item's
// rectangle is still in LastItemData); ItemAdd sees every item, labelled or not.
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& rect, const ImGuiLastItemData*) {
    ImGuiWindow* w = ctx->CurrentWindow;
    if (!uiitems::recording() || id == 0 || !w || w->IDStack.empty()) return;
    ImRect bb = rect;
    bb.ClipWith(w->ClipRect);
    if (bb.GetWidth() <= 0 || bb.GetHeight() <= 0) return;
    uiitems::anonCurrent.push_back({uiitems::windowName(w), id, w->IDStack.back(), bb.Min, bb.Max});
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext* ctx, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
    if (!uiitems::recording() || !label) return;
    const ImGuiLastItemData& last = ctx->LastItemData;
    if (last.ID != id) return;
    uiitems::record(ctx->CurrentWindow, label, last.Rect);
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
