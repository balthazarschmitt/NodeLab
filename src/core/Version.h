#pragma once
#include <string>

// The version comes from project(VERSION) in CMakeLists.txt; the commit hash is captured at build
// time (cmake/GitVersion.cmake), with "-dirty" when built from uncommitted changes.
extern const char* const kRefractoryVersion;    // "0.3.0"
extern const char* const kRefractoryCommit;     // "a1ccfa3" / "a1ccfa3-dirty" / "unknown"
extern const char* const kRefractoryBuildDate;  // __DATE__ of the generated file

// "0.3.0 (a1ccfa3, Sep 29 2026)"
inline std::string versionString() {
    return std::string(kRefractoryVersion) + " (" + kRefractoryCommit + ", " + kRefractoryBuildDate + ")";
}

// Compares dotted versions numerically ("0.10.0" > "0.9.2"). Missing parts count as 0.
inline int compareVersions(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && a[i] != '.') x = x * 10 + (a[i] >= '0' && a[i] <= '9' ? a[i] - '0' : 0), ++i;
        while (j < b.size() && b[j] != '.') y = y * 10 + (b[j] >= '0' && b[j] <= '9' ? b[j] - '0' : 0), ++j;
        if (x != y) return x < y ? -1 : 1;
        ++i, ++j;
    }
    return 0;
}
