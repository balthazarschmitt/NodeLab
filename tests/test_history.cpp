#include <doctest/doctest.h>

#include "graph/Graph.h"
#include "graph/History.h"
#include "nodes/group/GroupNodes.h"

TEST_CASE("History steps are named after what changed") {
    Graph g;
    const nlohmann::json empty = g.toJson();
    Node* blur = g.addNode("filter.blur");
    nlohmann::json one = g.toJson();
    CHECK(describeChange(empty, one) == "Add Blur");
    CHECK(describeChange(one, empty) == "Delete Blur");

    Node* blend = g.addNode("math.blend");
    Node* inv = g.addNode("color.invert");
    nlohmann::json three = g.toJson();
    CHECK(describeChange(one, three) == "Add 2 nodes");
    CHECK(describeChange(three, one) == "Delete 2 nodes");

    // One setting, with its value; an enum by its option's name; a bool as On/Off.
    blur->params[0] = 25.0f;
    nlohmann::json after = g.toJson();
    CHECK(describeChange(three, after) == "Blur: Size X 25.00");
    nlohmann::json before = after;
    blend->params[1] = 0;
    after = g.toJson();
    CHECK(describeChange(before, after) == "Blend: Mode Mix");
    before = after;
    blend->params[2] = false;
    after = g.toJson();
    CHECK(describeChange(before, after) == "Blend: Clamp Off");
    before = after;
    blur->params[0] = 5.0f, blur->params[1] = 5.0f;
    after = g.toJson();
    CHECK(describeChange(before, after) == "Blur: 2 settings");
    before = after;
    blur->params[0] = 6.0f, blend->params[0] = 0.5f;
    after = g.toJson();
    CHECK(describeChange(before, after) == "Edit 2 nodes");

    // Labels name the node; muting, wiring and moving.
    before = after;
    inv->label = "Negative";
    after = g.toJson();
    CHECK(describeChange(before, after) == "Rename Negative");
    before = after;
    inv->muted = true;
    after = g.toJson();
    CHECK(describeChange(before, after) == "Mute Negative");
    CHECK(describeChange(after, before) == "Unmute Negative");
    before = after;
    g.connect(blur->id, 0, inv->id, 0);
    after = g.toJson();
    CHECK(describeChange(before, after) == "Connect");
    CHECK(describeChange(after, before) == "Disconnect");
    before = after;
    g.connect(blend->id, 0, inv->id, 0);  // replaces the link into the same pin
    after = g.toJson();
    CHECK(describeChange(before, after) == "Reconnect");
    before = after;
    blur->x += 40;
    after = g.toJson();
    CHECK(describeChange(before, after) == "Move Blur");
    before = after;
    g.colorManagement.exposure = 1.0f;
    after = g.toJson();
    CHECK(describeChange(before, after) == "Color Management");
    CHECK(describeChange(after, after) == "Edit");
    Node* gamma = g.addNode("color.gamma");
    before = g.toJson();
    gamma->params[0] = 2.0f;
    CHECK(describeChange(before, g.toJson()) == "Gamma 2.00");  // not "Gamma: Gamma 2.00"
}

TEST_CASE("History names a change inside a group after the group") {
    Graph g;
    auto* group = static_cast<GroupNode*>(g.addNode("group.group"));
    REQUIRE(group);
    group->name = "Look";
    const nlohmann::json before = g.toJson();
    group->inner().addNode("color.invert");
    CHECK(describeChange(before, g.toJson()) == "Look: Add Invert");
}
