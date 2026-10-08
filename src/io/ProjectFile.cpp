#include "io/ProjectFile.h"

#include <fstream>

#include "core/Version.h"
#include "io/Paths.h"

namespace fs = std::filesystem;

bool saveProject(const std::string& pathU8, const Graph& g, const nlohmann::json& ui, std::string& err) {
    fs::path path = u8ToPath(pathU8);
    fs::path base = fs::absolute(path).parent_path();
    nlohmann::json j;
    j["app"] = "Refractory";
    j["version"] = g.colorManagement.linear ? kProjectVersion : 1;
    j["appVersion"] = kRefractoryVersion;
    j["graph"] = g.toJson(&base);
    j["ui"] = ui;

    // Write to a temp file then rename, so a crash mid-write never corrupts the project.
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            err = "cannot open file for writing";
            return false;
        }
        f << j.dump(2);
        if (!f) {
            err = "write failed";
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        err = ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool loadProject(const std::string& pathU8, Graph& g, nlohmann::json& ui, std::string& err) {
    fs::path path = u8ToPath(pathU8);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        err = "cannot open file";
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(f);
        if (!isProjectApp(j.value("app", ""))) {
            err = "not a Refractory project";
            return false;
        }
        if (j.value("version", 0) > kProjectVersion) {
            err = "project was saved by Refractory " + j.value("appVersion", std::string("(newer)")) +
                  ", which uses a newer file format than " + kRefractoryVersion + " can read";
            return false;
        }
        fs::path base = fs::absolute(path).parent_path();
        Graph loaded;
        loaded.fromJson(j.at("graph"), &base);
        g = std::move(loaded);
        ui = j.value("ui", nlohmann::json::object());
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

bool isProjectPath(const std::string& pathU8) {
    std::string ext = pathToU8(u8ToPath(pathU8).extension());
    for (char& c : ext) c = char(std::tolower((unsigned char)c));
    return ext == kProjectExt || ext == kLegacyProjectExt;
}
