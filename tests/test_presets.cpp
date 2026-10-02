#include <doctest/doctest.h>

#include <filesystem>

#include "io/Paths.h"
#include "io/Presets.h"

TEST_CASE("presets save, list, load and delete") {
    const auto dir = std::filesystem::temp_directory_path() / "nodelab_presets_test";
    std::filesystem::remove_all(dir);
    presets::setFolder(pathToU8(dir));
    nlohmann::json clip = {{"nodes", {{{"id", 3}, {"type", "color.invert"}, {"x", 0}, {"y", 0}}}},
                           {"links", nlohmann::json::array()}};
    std::string err;
    CHECK(presets::save("b look", clip, err));
    CHECK(presets::save("A: look", clip, err));  // ':' can't be in a file name
    CHECK_FALSE(presets::save("  ", clip, err));
    CHECK_FALSE(presets::save("empty", {{"nodes", nlohmann::json::array()}}, err));
    const auto names = presets::list();
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "A_ look");
    CHECK(names[1] == "b look");
    const nlohmann::json j = presets::load("b look");
    REQUIRE(j.is_object());
    CHECK(j["nodes"][0]["type"] == "color.invert");
    CHECK(presets::load("missing").is_null());
    CHECK(presets::remove("b look"));
    CHECK(presets::list().size() == 1);
    presets::setFolder("");
    std::filesystem::remove_all(dir);
}
