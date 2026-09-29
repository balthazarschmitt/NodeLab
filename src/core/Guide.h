#pragma once
#include <cstddef>
#include <string_view>

// GUIDE.md, compiled into the binary by cmake/EmbedText.cmake.
extern const char* const kGuideMarkdown;
extern const std::size_t kGuideMarkdownSize;

inline std::string_view guideMarkdown() { return {kGuideMarkdown, kGuideMarkdownSize}; }
