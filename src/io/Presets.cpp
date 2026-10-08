#include "io/Presets.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "io/Paths.h"

namespace fs = std::filesystem;

namespace presets {

namespace {

constexpr const char* kExt = ".rfpreset";
constexpr const char* kLegacyExt = ".nlpreset";  // NodeLab's presets (before 1.7)

std::mutex& folderMutex() {
    static std::mutex m;
    return m;
}
std::string& folderOverride() {
    static std::string s;
    return s;
}

std::string clean(std::string name) {
    for (char& c : name)
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos || (unsigned char)c < 32) c = '_';
    const auto b = name.find_first_not_of(" .");
    const auto e = name.find_last_not_of(" .");
    return b == std::string::npos ? std::string() : name.substr(b, e - b + 1);
}

fs::path fileFor(const std::string& name) { return u8ToPath(folder()) / u8ToPath(clean(name) + kExt); }
fs::path legacyFileFor(const std::string& name) { return u8ToPath(folder()) / u8ToPath(clean(name) + kLegacyExt); }

// The preset's file: its .rfpreset, or NodeLab's .nlpreset when that's the only one.
fs::path existingFileFor(const std::string& name) {
    const fs::path p = fileFor(name), old = legacyFileFor(name);
    std::error_code ec;
    return !fs::exists(p, ec) && fs::exists(old, ec) ? old : p;
}

}  // namespace

std::string folder() {
    std::lock_guard<std::mutex> lock(folderMutex());
    fs::path p;
    if (!folderOverride().empty()) {
        p = u8ToPath(folderOverride());
    } else {
#ifdef _WIN32
        if (const fs::path a = appDataDir(); !a.empty()) p = a / "presets";
#endif
        if (p.empty()) p = fs::current_path() / "presets";
    }
    std::error_code ec;
    fs::create_directories(p, ec);
    return pathToU8(p);
}

void setFolder(const std::string& dirU8) {
    std::lock_guard<std::mutex> lock(folderMutex());
    folderOverride() = dirU8;
}

std::vector<std::string> list() {
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(u8ToPath(folder()), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && (it->path().extension() == kExt || it->path().extension() == kLegacyExt))
            names.push_back(pathToU8(it->path().stem()));
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower((unsigned char)c));
        return s;
    };
    std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
    names.erase(std::unique(names.begin(), names.end()), names.end());  // in both formats
    return names;
}

bool save(const std::string& name, const nlohmann::json& clip, std::string& err) {
    if (clean(name).empty()) {
        err = "a preset needs a name";
        return false;
    }
    if (!clip.is_object() || !clip.contains("nodes") || clip["nodes"].empty()) {
        err = "nothing to save";
        return false;
    }
    const fs::path path = fileFor(name);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << nlohmann::json{{"refractoryPreset", 1}, {"nodes", clip["nodes"]}, {"links", clip.value("links", nlohmann::json::array())}}
                 .dump(1);
        if (!f) {
            err = "could not write " + pathToU8(tmp);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);  // atomic: a crash mid-write keeps the previous version
    if (ec) {
        fs::remove(tmp, ec);
        err = "could not save " + pathToU8(path);
        return false;
    }
    return true;
}

nlohmann::json load(const std::string& name) {
    std::vector<char> bytes;
    if (!readFileBytes(pathToU8(existingFileFor(name)), bytes)) return nullptr;
    nlohmann::json j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("nodes") || !j["nodes"].is_array()) return nullptr;
    if (!j.contains("links") || !j["links"].is_array()) j["links"] = nlohmann::json::array();
    return j;
}

bool remove(const std::string& name) {
    std::error_code ec;
    const bool removed = fs::remove(fileFor(name), ec);
    return fs::remove(legacyFileFor(name), ec) || removed;
}

}  // namespace presets
