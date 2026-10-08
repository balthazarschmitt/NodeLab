#pragma once
// Shared interface widgets built on ImGui, so every panel draws icon buttons, toggles, segmented
// controls, section headers and tooltips the same way. Icons come from ui/Icons.h.
//
// Script targets (UiItems.h): icon-only widgets answer to their name ("Before / After").
#include <imgui.h>

#include <initializer_list>
#include <string>

namespace ui {

// A square, borderless icon button the height of a frame. `on` draws it pressed (a toggle's
// state). The tooltip shows `name`, `key` (may be null) and `tip` (may be null).
bool IconButton(const char* icon, const char* name, bool on = false, const char* key = nullptr, const char* tip = nullptr);

// The accent-coloured button for a panel's main action ("Export").
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0));

// An on/off switch with its label to the right. Returns true when toggled.
bool Toggle(const char* label, bool* v);

// A row of joined buttons, one of them selected (view modes). `items` are labels or icons;
// `names` (optional) name icon-only items for tooltips and scripts. Returns true on change.
bool Segmented(const char* id, int* current, const char* const* items, int count, const char* const* names = nullptr);

// A panel section's header: a chevron, a semibold title and, on the right, optional bypass (eye)
// and reset buttons. Returns whether the section is open. `enabled` toggles with the eye;
// `reset` is set when the reset button is clicked.
bool SectionHeader(const char* title, bool* open, bool* enabled = nullptr, bool* reset = nullptr);

// A sub-heading inside a section ("WHITE BALANCE"): small caps-style dim text.
void SubHeading(const char* text);

// Tooltip for the last item: name and shortcut on the first line, a description below.
void Tooltip(const char* name, const char* key = nullptr, const char* text = nullptr);

// The theme's accent colour (primary buttons, toggles, selection marks).
ImU32 accent();

// The width a combo needs to show its widest item beside the arrow, at least `minWidth` (in
// unscaled pixels). Fixed widths clip items in other languages of size: larger UI scales grow
// the font but not a literal width. `items` is a list, or ImGui's "a\0b\0" form.
float comboWidth(const char* const* items, int count, float minWidth = 0.0f);
float comboWidth(const char* zeroSeparated, float minWidth = 0.0f);

// A floating window's first size, `w` x `h` unscaled pixels grown with the UI scale (so its
// contents fit at 150%), but never larger than 90% of the main window.
ImVec2 windowSize(float w, float h);

// `text` shortened with "..." to fit `maxWidth`, so a long name ends visibly rather than being
// cut through a letter.
std::string ellipsize(const std::string& text, float maxWidth);

// A dialog's button row, the same in every dialog: a separator, then buttons of one width with
// the main action first and Cancel last. Enter presses the first (unless a text field has the
// keyboard or it's disabled), Escape the last. Returns the index pressed, or -1.
int dialogButtons(std::initializer_list<const char*> labels, bool firstEnabled = true);

}  // namespace ui
