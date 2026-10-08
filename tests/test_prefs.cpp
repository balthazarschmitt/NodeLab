// Preferences: colour themes (JSON round trip, presets) and Reset to Defaults.
#include <doctest/doctest.h>

#include <set>

#include "graph/Graph.h"
#include "nodes/io/IONodes.h"
#include "ui/Theme.h"

TEST_CASE("theme presets: Studio first, then Classic; unique names, every colour set") {
    const auto& p = theme::presets();
    REQUIRE(p.size() >= 6);
    CHECK(p[0].name == "Studio");
    CHECK(p[0].uiSet[theme::Control]);
    // Classic overrides nothing, so it is exactly ImGui's dark style, the look before 1.4.
    CHECK(p[1].name == "Classic");
    for (int k = 0; k < theme::kUiKeys; ++k) CHECK_FALSE(p[1].uiSet[k]);
    std::set<std::string> names;
    for (const theme::Theme& t : p) {
        names.insert(t.name);
        for (int c = 0; c < theme::kCols; ++c) CHECK(t.col[c] != 0);
    }
    CHECK(names.size() == p.size());
}

TEST_CASE("theme JSON round trip keeps every colour") {
    for (const theme::Theme& t : theme::presets()) {
        const theme::Theme back = theme::Theme::fromJson(t.toJson());
        CHECK(back.name == t.name);
        CHECK(back.light == t.light);
        for (int k = 0; k < theme::kUiKeys; ++k) {
            CHECK(back.uiSet[k] == t.uiSet[k]);
            if (t.uiSet[k]) CHECK(back.ui[k].x == doctest::Approx(t.ui[k].x));
        }
        for (int c = 0; c < theme::kCols; ++c) CHECK(back.col[c] == t.col[c]);
    }
    // Missing entries (a theme saved by an older build) come from Classic, which they were made on.
    const theme::Theme partial = theme::Theme::fromJson({{"name", "Mine"}, {"colors", {{"canvas", {1, 0, 0}}}}});
    CHECK(partial.name == "Mine");
    CHECK(partial.col[theme::Canvas] == IM_COL32(255, 0, 0, 255));
    CHECK(partial.col[theme::Grid] == theme::presets()[1].col[theme::Grid]);
    CHECK_FALSE(partial.uiSet[theme::Control]);
    CHECK(theme::Theme::fromJson("junk").name == "Classic");
}

TEST_CASE("saved built-in themes load the current preset") {
    // Preferences saved before 1.4 hold the old default, Refractory Dark: they get the new default.
    nlohmann::json old = theme::presets()[1].toJson();
    old["name"] = "Refractory Dark";
    CHECK(theme::Theme::fromJson(old).name == "Studio");
    // A built-in theme saved by an older build follows the preset's updates.
    nlohmann::json blender = {{"name", "Blender"}, {"colors", {{"canvas", {1, 0, 0}}}}};
    const theme::Theme b = theme::Theme::fromJson(blender);
    for (const theme::Theme& t : theme::presets())
        if (t.name == "Blender") CHECK(b.col[theme::Canvas] == t.col[theme::Canvas]);
}

TEST_CASE("Reset to Defaults keeps the file and a RAW's defaults") {
    Graph g;
    Node* blur = g.addNode("filter.blur");
    REQUIRE(blur);
    const auto defaults = blur->params;
    blur->params[0] = 42.0f;
    blur->resetParams();
    CHECK(blur->params == defaults);

    auto& in = static_cast<ImageInputNode&>(*g.addNode("io.image_input"));
    in.chooseFile("photo.jpg");
    in.params[1] = 1;  // Color Space
    in.resetParams();
    CHECK(in.paramS(0) == "photo.jpg");
    CHECK(in.paramI(1) == 0);

    in.params[0] = "";
    in.chooseFile("photo.CR2");
    in.params[3] = -1.0f;
    in.params[4] = false;
    in.resetParams();
    CHECK(in.paramS(0) == "photo.CR2");
    CHECK(in.paramF(3) == doctest::Approx(ImageInputNode::kRawBaselineEV));
    CHECK(in.paramB(4));
}
