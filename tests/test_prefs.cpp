// Preferences: colour themes (JSON round trip, presets) and Reset to Defaults.
#include <doctest/doctest.h>

#include <set>

#include "graph/Graph.h"
#include "nodes/io/IONodes.h"
#include "ui/Theme.h"

TEST_CASE("theme presets: NodeLab Dark first, unique names, every colour set") {
    const auto& p = theme::presets();
    REQUIRE(p.size() >= 5);
    CHECK(p[0].name == "NodeLab Dark");
    // The default theme overrides nothing, so the UI is exactly ImGui's dark style as before.
    for (int k = 0; k < theme::kUiKeys; ++k) CHECK_FALSE(p[0].uiSet[k]);
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
    // Missing entries (a theme saved by an older build) come from the default theme.
    const theme::Theme partial = theme::Theme::fromJson({{"name", "Mine"}, {"colors", {{"canvas", {1, 0, 0}}}}});
    CHECK(partial.name == "Mine");
    CHECK(partial.col[theme::Canvas] == IM_COL32(255, 0, 0, 255));
    CHECK(partial.col[theme::Grid] == theme::presets()[0].col[theme::Grid]);
    CHECK(theme::Theme::fromJson("junk").name == "NodeLab Dark");
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
