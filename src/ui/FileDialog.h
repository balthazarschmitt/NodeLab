#pragma once
#include <optional>
#include <string>

// Native Windows file dialogs. `filter` uses the Win32 double-null format written with '|'
// separators, e.g. "Images|*.png;*.jpg|All files|*.*". Returned paths are UTF-8.
std::optional<std::string> openFileDialog(const char* title, const char* filter);
std::optional<std::string> saveFileDialog(const char* title, const char* filter, const char* defaultExt);
