#include "ui/Style.h"

#include <cstddef>
#include <filesystem>
#include <string_view>

#include "core/Guide.h"
#include "ui/GuideWindow.h"
#include "ui/Icons.h"

// Lucide subset (third_party/lucide), embedded by cmake/EmbedText.cmake.
extern const char* const kFontLucide;
extern const std::size_t kFontLucideSize;

namespace style {
namespace {

Fonts gFonts;
float gUiScale = 1.0f;
float gDpi = 1.0f;
bool gDirty = true;

bool exists(const char* file) {
    std::error_code ec;
    return std::filesystem::exists(file, ec);
}

// Merges the icons into the font just added, centred on its line. Lucide's line box is its em, so
// an icon `px` tall sits (line - px) / 2 below the line's top.
void mergeIcons(float line, float px) {
    static const ImWchar ranges[] = {ICON_MIN, ICON_MAX, 0};
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.FontDataOwnedByAtlas = false;  // static data
    cfg.PixelSnapH = true;
    cfg.GlyphMinAdvanceX = px;
    cfg.GlyphOffset = ImVec2(0, (line - px) * 0.5f);
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<char*>(kFontLucide), int(kFontLucideSize), px, &cfg, ranges);
}

}  // namespace

const Fonts& fonts() { return gFonts; }

float uiScale() { return gUiScale; }

void setUiScale(float s) {
    s = s < 0.5f ? 0.5f : s > 3.0f ? 3.0f : s;
    if (s == gUiScale) return;
    gUiScale = s;
    gDirty = true;
}

float scale() { return gDpi * gUiScale; }

bool fontsDirty() { return gDirty; }

void build(float dpi) {
    gDpi = dpi < 1.0f ? 1.0f : dpi;
    gDirty = false;
    const float s = scale();
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    gFonts = {};

    // Segoe UI Variable (Windows 11's UI font), or Segoe UI where it's missing (Windows 10). It's
    // a variable font and ImGui's rasterizer reads only its default instance, Text Regular, which
    // is the one Windows uses for UI text.
    const char* regular = exists("C:/Windows/Fonts/SegUIVar.ttf") ? "C:/Windows/Fonts/SegUIVar.ttf" : "C:/Windows/Fonts/segoeui.ttf";
    const char* semibold = "C:/Windows/Fonts/seguisb.ttf";
    auto face = [&](const char* file, float px, bool icons) -> ImFont* {
        ImFont* f = exists(file) ? io.Fonts->AddFontFromFileTTF(file, px * s) : nullptr;
        if (!f) {
            ImFontConfig cfg;
            cfg.SizePixels = px * s;
            f = io.Fonts->AddFontDefault(&cfg);
        }
        if (icons) mergeIcons(px * s, (px - 2.0f) * s);
        return f;
    };
    gFonts.body = face(regular, 17.0f, true);  // first: ImGui's default font
    gFonts.small = face(regular, 15.0f, true);
    gFonts.semibold = exists(semibold) ? face(semibold, 17.0f, true) : gFonts.body;
    gFonts.heading = exists(semibold) ? face(semibold, 20.0f, false) : gFonts.body;

    // Extra faces for the guide: bold, headings and code. Glyphs cover the guide's own text.
    {
        static ImVector<ImWchar> ranges;
        if (ranges.empty()) {
            ImFontGlyphRangesBuilder rb;
            rb.AddRanges(io.Fonts->GetGlyphRangesDefault());
            const std::string_view guide = guideMarkdown();
            rb.AddText(guide.data(), guide.data() + guide.size());
            rb.BuildRanges(&ranges);
        }
        auto load = [&](const char* file, float size) -> ImFont* {
            return exists(file) ? io.Fonts->AddFontFromFileTTF(file, size * s, nullptr, ranges.Data) : nullptr;
        };
        GuideFonts gf;
        gf.bold = load("C:/Windows/Fonts/segoeuib.ttf", 17.0f);
        gf.h1 = load("C:/Windows/Fonts/segoeuib.ttf", 30.0f);
        gf.h2 = load("C:/Windows/Fonts/segoeuib.ttf", 24.0f);
        gf.h3 = load("C:/Windows/Fonts/segoeuib.ttf", 20.0f);
        gf.code = load("C:/Windows/Fonts/consola.ttf", 16.0f);
        setGuideFonts(gf);
    }
    io.Fonts->Build();
    applyMetrics();
}

void applyMetrics() {
    // Reset the sizes (not the colours) and scale them once: ScaleAllSizes multiplies what's there.
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4 colors[ImGuiCol_COUNT];
    for (int i = 0; i < ImGuiCol_COUNT; ++i) colors[i] = style.Colors[i];
    style = ImGuiStyle();
    for (int i = 0; i < ImGuiCol_COUNT; ++i) style.Colors[i] = colors[i];

    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(6, 3);
    style.ItemSpacing = ImVec2(8, 4);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.CellPadding = ImVec2(6, 3);
    style.IndentSpacing = 16;
    style.ScrollbarSize = 12;
    style.GrabMinSize = 10;
    style.WindowRounding = 0;
    style.ChildRounding = 4;
    style.FrameRounding = 4;
    style.PopupRounding = 6;
    style.ScrollbarRounding = 6;
    style.GrabRounding = 3;
    style.TabRounding = 4;
    style.WindowBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    style.TabBarBorderSize = 1;
    style.DockingSeparatorSize = 1;
    style.SeparatorTextBorderSize = 1;
    style.SeparatorTextPadding = ImVec2(0, 4);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ScaleAllSizes(scale());
}

}  // namespace style
