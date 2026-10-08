#include "ui/GuideWindow.h"
#include "ui/Widgets.h"
#include "ui/Style.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "core/Guide.h"

namespace {

enum class Kind { Heading, Para, Bullet, Numbered, Table };

struct Block {
    Kind kind = Kind::Para;
    int level = 0;       // heading level (4 = a bold-only line), or list nesting depth
    std::string text;    // inline markdown
    std::string number;  // numbered list: "1."
    std::vector<std::vector<std::string>> rows;  // table: first row is the header
};

// A heading and the blocks under it, up to the next heading of any level.
struct Section {
    std::string title;  // plain text
    int level = 1;
    int first = 0, end = 0;  // blocks [first, end); first is the heading itself
    int parent = -1;
    std::vector<int> children;
    std::string haystack;  // lowercase plain text, for search
};

enum class Style { Normal, Bold, Code };
struct Span {
    std::string text;
    Style style;
};

struct Guide {
    std::vector<Block> blocks;
    std::vector<Section> sections;
};

struct State {
    bool open = false;
    bool focus = false;
    GuideFonts fonts;
    Guide guide;
    bool parsed = false;
    char search[128] = "";
    std::string lastSearch;  // the body scrolls back to the top when the search changes
    int scrollTo = -1;     // section to bring to the top of the body
    int scrollFrames = 0;  // re-applied for a few frames: text rewraps while a new window sizes itself
    int current = -1;      // section at the top of the body (highlighted in the contents)
    int revealTo = -1;     // open the contents tree down to this section once
    float maxRight = FLT_MAX;  // screen x where body text wraps at the latest
};
State& st() {
    static State s;
    return s;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string_view trimLeft(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    return s;
}

std::string trim(std::string_view s) {
    s = trimLeft(s);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return std::string(s);
}

std::vector<Span> parseInline(std::string_view t) {
    std::vector<Span> out;
    Style style = Style::Normal;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) out.push_back({cur, style});
        cur.clear();
    };
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (style == Style::Code) {
            if (c == '`') {
                flush();
                style = Style::Normal;
            } else {
                cur += c;
            }
        } else if (c == '\\' && i + 1 < t.size() && std::ispunct(static_cast<unsigned char>(t[i + 1]))) {
            cur += t[++i];
        } else if (c == '`') {
            flush();
            style = Style::Code;
        } else if (c == '*' && i + 1 < t.size() && t[i + 1] == '*') {
            flush();
            style = style == Style::Bold ? Style::Normal : Style::Bold;
            ++i;
        } else {
            cur += c;
        }
    }
    flush();
    return out;
}

std::string plain(std::string_view t) {
    std::string r;
    for (const Span& s : parseInline(t)) r += s.text;
    return r;
}

std::vector<std::string> splitRow(std::string_view line) {
    line = trimLeft(line);
    if (!line.empty() && line.front() == '|') line.remove_prefix(1);
    while (!line.empty() && (line.back() == ' ' || line.back() == '|')) line.remove_suffix(1);
    std::vector<std::string> cells;
    size_t start = 0;
    for (size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || (line[i] == '|' && (i == 0 || line[i - 1] != '\\'))) {
            cells.push_back(trim(line.substr(start, i - start)));
            start = i + 1;
        }
    }
    return cells;
}

bool isSeparatorRow(const std::vector<std::string>& cells) {
    for (const std::string& c : cells)
        if (c.find_first_not_of("-: ") != std::string::npos) return false;
    return true;
}

Guide parseGuide(std::string_view md) {
    Guide g;
    bool blank = true;
    size_t pos = 0;
    while (pos <= md.size()) {
        size_t nl = md.find('\n', pos);
        if (nl == std::string_view::npos) nl = md.size();
        std::string_view line = md.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        std::string_view t = trimLeft(line);
        const int indent = int(line.size() - t.size());
        if (t.empty()) {
            blank = true;
            continue;
        }
        Block* last = g.blocks.empty() ? nullptr : &g.blocks.back();
        size_t digits = 0;
        while (digits < t.size() && std::isdigit(static_cast<unsigned char>(t[digits]))) ++digits;

        if (indent == 0 && t.front() == '#') {
            Block b{Kind::Heading};
            while (b.level < int(t.size()) && t[b.level] == '#') ++b.level;
            b.text = trim(t.substr(b.level));
            g.blocks.push_back(std::move(b));
        } else if (t.front() == '|') {
            auto cells = splitRow(t);
            if (isSeparatorRow(cells)) continue;
            if (last && last->kind == Kind::Table && !blank) {
                last->rows.push_back(std::move(cells));
            } else {
                Block b{Kind::Table};
                b.rows.push_back(std::move(cells));
                g.blocks.push_back(std::move(b));
            }
        } else if (t.starts_with("- ")) {
            g.blocks.push_back(Block{Kind::Bullet, indent / 2, std::string(t.substr(2))});
        } else if (digits > 0 && t.substr(digits).starts_with(". ")) {
            Block b{Kind::Numbered, indent / 2, std::string(t.substr(digits + 2))};
            b.number = std::string(t.substr(0, digits + 1));
            g.blocks.push_back(std::move(b));
        } else if (indent == 0 && t.size() > 4 && t.starts_with("**") && t.find("**", 2) == t.size() - 2) {
            // A line that is only bold text titles an entry (every node has one).
            g.blocks.push_back(Block{Kind::Heading, 4, std::string(t.substr(2, t.size() - 4))});
        } else if (!blank && last && (last->kind == Kind::Para || last->kind == Kind::Bullet || last->kind == Kind::Numbered)) {
            last->text += ' ';
            last->text += t;  // continuation line
        } else {
            g.blocks.push_back(Block{Kind::Para, 0, std::string(t)});
        }
        blank = false;
    }

    // Sections, nested by heading level.
    std::vector<int> stack;
    for (int i = 0; i < int(g.blocks.size()); ++i) {
        const Block& b = g.blocks[i];
        if (b.kind != Kind::Heading) {
            if (!g.sections.empty()) g.sections.back().end = i + 1;
            continue;
        }
        Section s;
        s.title = plain(b.text);
        s.level = b.level;
        s.first = i;
        s.end = i + 1;
        while (!stack.empty() && g.sections[stack.back()].level >= b.level) stack.pop_back();
        s.parent = stack.empty() ? -1 : stack.back();
        const int idx = int(g.sections.size());
        if (s.parent >= 0) g.sections[s.parent].children.push_back(idx);
        g.sections.push_back(std::move(s));
        stack.push_back(idx);
    }
    for (Section& s : g.sections) {
        std::string text;
        for (int i = s.first; i < s.end; ++i) {
            text += plain(g.blocks[i].text) + '\n';
            for (auto& row : g.blocks[i].rows)
                for (auto& c : row) text += plain(c) + '\n';
        }
        s.haystack = lower(text);
    }
    return g;
}

void ensureParsed() {
    State& s = st();
    if (s.parsed) return;
    s.guide = parseGuide(guideMarkdown());
    s.parsed = true;
}

// Search terms: every whitespace-separated word must appear.
std::vector<std::string> searchTerms() {
    std::vector<std::string> terms;
    std::string q = lower(st().search), cur;
    for (char c : q + ' ') {
        if (c == ' ') {
            if (!cur.empty()) terms.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return terms;
}

bool matches(const Section& s, const std::vector<std::string>& terms) {
    for (const std::string& t : terms)
        if (s.haystack.find(t) == std::string::npos) return false;
    return true;
}

// ---------------------------------------------------------------- rich text

ImFont* orBase(ImFont* f, ImFont* base) { return f ? f : base; }

// Draws inline markdown word by word, wrapping at the content region's right edge. Words that
// contain a search term get a highlight behind them.
void drawRich(std::string_view text, ImFont* base, ImU32 col, const std::vector<std::string>& terms) {
    const GuideFonts& f = st().fonts;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float right = start.x + std::max(50.0f, std::min(ImGui::GetContentRegionAvail().x, st().maxRight - start.x));
    const float lineH = base->FontSize + 3.0f;
    float x = start.x, y = start.y;

    for (const Span& span : parseInline(text)) {
        ImFont* font = span.style == Style::Code ? orBase(f.code, base)
                       : span.style == Style::Bold && base != f.h1 && base != f.h2 && base != f.h3
                           ? orBase(f.bold, base)
                           : base;
        const float yOff = (base->FontSize - font->FontSize) * 0.5f;
        // Words keep their trailing spaces; code spans never break.
        size_t i = 0;
        const std::string& s = span.text;
        while (i < s.size()) {
            size_t wordEnd = span.style == Style::Code ? s.size() : s.find(' ', i);
            if (wordEnd == std::string::npos) wordEnd = s.size();
            size_t next = wordEnd;
            while (next < s.size() && s[next] == ' ') ++next;
            const char* b = s.data() + i;
            const float w = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, b, s.data() + wordEnd).x;
            const float full = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0, b, s.data() + next).x;
            if (x > start.x && x + w > right) {
                x = start.x;
                y += lineH;
            }
            const ImVec2 p(x, y + yOff);
            if (!terms.empty()) {
                const std::string word = lower(std::string_view(b, wordEnd - i));
                for (const std::string& t : terms)
                    if (word.find(t) != std::string::npos) {
                        dl->AddRectFilled(ImVec2(p.x - 1, y), ImVec2(p.x + w + 1, y + lineH - 1), IM_COL32(200, 160, 40, 110), 2);
                        break;
                    }
            }
            if (span.style == Style::Code) {
                dl->AddRectFilled(ImVec2(p.x - 2, y), ImVec2(p.x + w + 2, y + lineH - 1), IM_COL32(255, 255, 255, 22), 3);
                dl->AddText(font, font->FontSize, p, IM_COL32(236, 196, 128, 255), b, s.data() + wordEnd);
            } else {
                dl->AddText(font, font->FontSize, p, col, b, s.data() + wordEnd);
            }
            x += full;
            i = next;
        }
    }
    ImGui::Dummy(ImVec2(right - start.x, y + lineH - start.y));
}

void drawBlock(const Block& b, int blockIndex, const std::vector<std::string>& terms) {
    const GuideFonts& f = st().fonts;
    ImFont* body = ImGui::GetFont();
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    switch (b.kind) {
        case Kind::Heading: {
            ImFont* font = b.level == 1 ? orBase(f.h1, body) : b.level == 2 ? orBase(f.h2, body)
                         : b.level == 3 ? orBase(f.h3, body) : orBase(f.bold, body);
            const ImU32 col = b.level == 4 ? IM_COL32(140, 190, 255, 255) : IM_COL32(242, 200, 110, 255);
            ImGui::Dummy(ImVec2(0, b.level <= 2 ? 14.0f : b.level == 3 ? 8.0f : 4.0f));
            drawRich(b.text, font, col, terms);
            if (b.level <= 2) ImGui::Separator();
            break;
        }
        case Kind::Para:
            drawRich(b.text, body, text, terms);
            ImGui::Dummy(ImVec2(0, 2));
            break;
        case Kind::Bullet:
        case Kind::Numbered: {
            const float step = body->FontSize * 1.3f;
            const float indent = step * float(b.level + 1);
            ImGui::Indent(indent);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (b.kind == Kind::Bullet) {
                dl->AddCircleFilled(ImVec2(p.x - step * 0.5f, p.y + body->FontSize * 0.55f), 2.5f,
                                    b.level ? IM_COL32(150, 150, 160, 255) : text);
            } else {
                const float w = ImGui::CalcTextSize(b.number.c_str()).x;
                dl->AddText(ImVec2(p.x - w - 5, p.y), text, b.number.c_str());
            }
            drawRich(b.text, body, text, terms);
            ImGui::Unindent(indent);
            break;
        }
        case Kind::Table: {
            const int cols = int(b.rows.front().size());
            ImGui::PushID(blockIndex);
            const float width = std::min(ImGui::GetContentRegionAvail().x, st().maxRight - ImGui::GetCursorScreenPos().x);
            if (ImGui::BeginTable("##table", cols, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(std::max(100.0f, width), 0))) {
                // Column widths follow the longest cell, so short key columns stay narrow.
                for (int c = 0; c < cols; ++c) {
                    size_t longest = 4;
                    for (auto& row : b.rows)
                        if (c < int(row.size())) longest = std::max(longest, std::min<size_t>(plain(row[c]).size(), 70));
                    ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, float(longest));
                }
                for (size_t r = 0; r < b.rows.size(); ++r) {
                    ImGui::TableNextRow(r == 0 ? ImGuiTableRowFlags_Headers : 0);
                    if (r == 0) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
                    for (int c = 0; c < cols; ++c) {
                        ImGui::TableSetColumnIndex(c);
                        if (c < int(b.rows[r].size())) drawRich(b.rows[r][c], r == 0 ? orBase(f.bold, body) : body, text, terms);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 4));
            break;
        }
    }
}

void jumpTo(int section) {
    st().scrollTo = section;
    st().scrollFrames = 3;
}

// ---------------------------------------------------------------- contents

void drawTocEntry(int i) {
    State& s = st();
    const Section& sec = s.guide.sections[i];
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (sec.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (i == s.current) flags |= ImGuiTreeNodeFlags_Selected;
    // Jumping to a section opens its ancestors so it shows up in the tree.
    if (s.revealTo >= 0 && !sec.children.empty()) {
        for (int a = s.revealTo; a >= 0; a = s.guide.sections[a].parent)
            if (a == i) ImGui::SetNextItemOpen(true);
    }
    ImGui::PushID(i);
    const bool open = ImGui::TreeNodeEx("##toc", flags, "%s", sec.title.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) jumpTo(i);
    ImGui::PopID();
    if (open && !sec.children.empty()) {
        for (int c : sec.children) drawTocEntry(c);
        ImGui::TreePop();
    }
}

}  // namespace

void setGuideFonts(const GuideFonts& fonts) { st().fonts = fonts; }

bool guideOpen() { return st().open; }

void openGuide(const std::string& topic) {
    State& s = st();
    ensureParsed();
    s.open = true;
    s.focus = true;
    if (topic.empty()) return;
    // Prefer the deepest heading with that exact title (a node entry over a category).
    const std::string want = lower(topic);
    int best = -1;
    auto titled = [&](const std::string& title) {
        // "Split HSV / Combine HSV" documents both nodes.
        const std::string t = lower(title);
        if (t == want) return true;
        for (size_t a = 0, e; a <= t.size(); a = e + 3) {
            e = t.find(" / ", a);
            if (e == std::string::npos) e = t.size();
            if (t.compare(a, e - a, want) == 0 && e - a == want.size()) return true;
        }
        return false;
    };
    for (int i = 0; i < int(s.guide.sections.size()); ++i)
        if (titled(s.guide.sections[i].title) && (best < 0 || s.guide.sections[i].level > s.guide.sections[best].level))
            best = i;
    if (best >= 0) {
        s.search[0] = '\0';
        jumpTo(best);
        s.revealTo = best;
    } else {
        std::snprintf(s.search, sizeof(s.search), "%s", topic.c_str());
    }
}

void drawGuideWindow() {
    State& s = st();
    if (!s.open) return;
    ensureParsed();
    ImGui::SetNextWindowSize(ui::windowSize(960, 700), ImGuiCond_FirstUseEver);
    if (s.focus) {
        ImGui::SetNextWindowFocus();
        s.focus = false;
    }
    if (!ImGui::Begin("Guide###Guide", &s.open)) {
        ImGui::End();
        return;
    }
    const auto terms = searchTerms();
    const auto& sections = s.guide.sections;

    ImGui::SetNextItemWidth(340 * style::scale());
    ImGui::InputTextWithHint("##search", "Search (e.g. saturation, hsv, green screen)", s.search, sizeof(s.search));
    int matchCount = 0;
    if (!terms.empty()) {
        for (const Section& sec : sections) matchCount += matches(sec, terms);
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) s.search[0] = '\0';
        ImGui::SameLine();
        ImGui::TextDisabled("%d matching section%s", matchCount, matchCount == 1 ? "" : "s");
    }

    ImGui::BeginChild("##contents", ImVec2(270 * style::scale(), 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
    if (!terms.empty()) {
        for (int i = 0; i < int(sections.size()); ++i) {
            if (!matches(sections[i], terms)) continue;
            ImGui::PushID(i);
            if (ImGui::Selectable(sections[i].title.c_str(), i == s.current)) jumpTo(i);
            ImGui::PopID();
        }
    } else {
        for (int i = 0; i < int(sections.size()); ++i) {
            if (sections[i].parent >= 0 && sections[i].level > 1 && sections[sections[i].parent].level > 1) continue;
            // The title section is a plain entry; its chapters sit at the top level.
            if (sections[i].level == 1) {
                ImGui::PushID(i);
                if (ImGui::Selectable(sections[i].title.c_str(), i == s.current)) jumpTo(i);
                ImGui::PopID();
            } else {
                drawTocEntry(i);
            }
        }
    }
    s.revealTo = -1;
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##body", ImVec2(0, 0), ImGuiChildFlags_None);
    if (s.lastSearch != s.search) {
        s.lastSearch = s.search;
        if (s.scrollTo < 0) ImGui::SetScrollY(0.0f);
    }
    const float top = ImGui::GetWindowPos().y;
    // Long lines are hard to read on wide screens, so text stops at a fixed column.
    s.maxRight = ImGui::GetCursorScreenPos().x + 920.0f;
    int current = -1;
    if (!terms.empty() && matchCount == 0) ImGui::TextDisabled("Nothing matches \"%s\".", s.search);
    for (int i = 0; i < int(sections.size()); ++i) {
        if (!terms.empty() && !matches(sections[i], terms)) continue;
        const Section& sec = sections[i];
        ImGui::PushID(i);
        for (int b = sec.first; b < sec.end; ++b) {
            if (b == sec.first && ImGui::GetCursorScreenPos().y <= top + 30) current = i;
            if (b == sec.first && i == s.scrollTo) {
                ImGui::SetScrollY(ImGui::GetCursorPosY());  // content coordinates of the heading
                if (--s.scrollFrames <= 0) s.scrollTo = -1;
            }
            drawBlock(s.guide.blocks[b], b, terms);
        }
        ImGui::PopID();
    }
    s.current = current < 0 && !sections.empty() ? 0 : current;
    ImGui::Dummy(ImVec2(0, ImGui::GetWindowHeight() * 0.6f));  // lets the last sections scroll to the top
    ImGui::EndChild();
    ImGui::End();
}
