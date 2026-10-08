#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "core/Version.h"
#include "graph/Graph.h"
#include "io/ProjectFile.h"

TEST_CASE("version comparison is numeric") {
    CHECK(compareVersions("0.3.0", "0.3.0") == 0);
    CHECK(compareVersions("0.10.0", "0.9.2") == 1);
    CHECK(compareVersions("0.3", "0.3.1") == -1);
    CHECK(compareVersions("1.0.0", "0.99.99") == 1);
}

TEST_CASE("projects record the app version and reject newer formats") {
    namespace fs = std::filesystem;
    fs::path p = fs::temp_directory_path() / "refractory_version_test.refract";
    Graph g;
    g.addNode("io.output");
    std::string err;
    REQUIRE(saveProject(p.string(), g, {}, err));

    Graph loaded;
    nlohmann::json ui;
    REQUIRE(loadProject(p.string(), loaded, ui, err));
    {
        std::ifstream f(p);
        nlohmann::json j = nlohmann::json::parse(f);
        CHECK(j["appVersion"] == kRefractoryVersion);
        j["version"] = kProjectVersion + 1;
        j["appVersion"] = "9.0.0";
        std::ofstream(p) << j.dump();
    }
    CHECK_FALSE(loadProject(p.string(), loaded, ui, err));
    CHECK(err.find("9.0.0") != std::string::npos);
    fs::remove(p);
}
