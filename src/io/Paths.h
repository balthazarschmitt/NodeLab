#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

// The whole file in one read; false if it can't be opened or read. (Copying through
// istreambuf_iterator goes a byte at a time: about 0.1 s for a 30 MB RAW.)
inline bool readFileBytes(const std::string& pathU8, std::vector<char>& out) {
    std::ifstream f(u8ToPath(pathU8), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    out.resize(size_t(size));
    f.seekg(0);
    return size == 0 || bool(f.read(out.data(), size));
}

inline std::string makeAbsoluteU8(const std::string& path, const std::filesystem::path& baseDir) {
    auto p = u8ToPath(path);
    if (p.is_absolute()) return path;
    return pathToU8((baseDir / p).lexically_normal());
}
