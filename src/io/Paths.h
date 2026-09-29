#pragma once
#include <filesystem>
#include <string>

// All paths inside the app are UTF-8 std::strings; convert at filesystem boundaries.

inline std::filesystem::path u8ToPath(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

inline std::string pathToU8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

inline std::string makeRelativeU8(const std::string& absPath, const std::filesystem::path& baseDir) {
    std::error_code ec;
    auto rel = std::filesystem::relative(u8ToPath(absPath), baseDir, ec);
    if (ec || rel.empty()) return absPath;
    return pathToU8(rel.generic_u8string());
}

inline std::string makeAbsoluteU8(const std::string& path, const std::filesystem::path& baseDir) {
    auto p = u8ToPath(path);
    if (p.is_absolute()) return path;
    return pathToU8((baseDir / p).lexically_normal());
}
