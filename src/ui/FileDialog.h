#pragma once
#include <optional>
#include <string>
#include <vector>

// Native Windows file dialogs. `filter` uses the Win32 double-null format written with '|'
// separators, e.g. "Images|*.png;*.jpg|All files|*.*". Returned paths are UTF-8.
std::optional<std::string> openFileDialog(const char* title, const char* filter);
std::optional<std::string> saveFileDialog(const char* title, const char* filter, const char* defaultExt);
// Open dialog allowing several files; empty when cancelled.
std::vector<std::string> openFilesDialog(const char* title, const char* filter);
std::optional<std::string> folderDialog(const char* title);

class Node;
// Sets a Path param to a file chosen in the UI. An Image Input given a RAW after no file or a
// non-RAW one gets the RAW defaults (ImageInputNode::chooseFile); takeRawChosen() then reports it
// once, so the App can switch a fresh project's view to AgX.
void chooseImageFile(Node& n, int param, const std::string& pathU8);
bool takeRawChosen();
