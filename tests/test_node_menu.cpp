#include <doctest/doctest.h>

#include <map>

#include "graph/NodeMenu.h"
#include "graph/NodeRegistry.h"

TEST_CASE("the add menu lists every visible node once, in sections that fit") {
    const auto& reg = NodeRegistry::instance();
    std::map<std::string, int> seen;
    for (const auto& m : nodemenu::menus()) {
        CAPTURE(m.name);
        REQUIRE(!m.items.empty());
        CHECK(!m.items.front().empty());
        CHECK(!m.items.back().empty());
        int rows = 0;
        for (size_t i = 0; i < m.items.size(); ++i) {
            if (m.items[i].empty()) {
                CHECK(!m.items[i - 1].empty());  // no doubled separators
                continue;
            }
            ++rows;
            ++seen[m.items[i]];
            CHECK(nodemenu::menuOf(m.items[i]) == m.name);
        }
        // About what fits under the cursor on a 900-pixel-high window without scrolling.
        CHECK(rows <= 20);
    }
    for (const auto& t : reg.types()) {
        CAPTURE(t);
        CHECK(seen[t] == (reg.find(t)->hidden ? 0 : 1));
    }
}
