#pragma once
#include <string>

struct ImFont;

// Help > Guide: shows the embedded GUIDE.md in a dockable window with a contents tree and a
// search box. Only the markdown the guide uses is understood: #/##/### headings, paragraphs,
// "- " bullets (nested by 2 spaces), "1. " lists, tables, **bold**, `code` and \* escapes.
// A line that is only **bold** (a node's name) becomes a small heading so it can be jumped to.

struct GuideFonts {
    ImFont* bold = nullptr;
    ImFont* h1 = nullptr;
    ImFont* h2 = nullptr;
    ImFont* h3 = nullptr;
    ImFont* code = nullptr;
};
void setGuideFonts(const GuideFonts& fonts);

// Opens the guide; with a topic (a heading such as a node's display name) it scrolls there.
void openGuide(const std::string& topic = {});
bool guideOpen();
void drawGuideWindow();
