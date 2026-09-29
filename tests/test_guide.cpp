#include <doctest/doctest.h>

#include <set>
#include <string>

#include "core/Guide.h"
#include "graph/NodeRegistry.h"

TEST_CASE("the embedded guide documents every node") {
    const std::string_view guide = guideMarkdown();
    REQUIRE(guide.size() > 1000);
    CHECK(guide.substr(0, 15) == "# NodeLab Guide");

    // Node entries are titled by a line that is only bold text; "Split HSV / Combine HSV" covers
    // both nodes (Help > Guide jumps to it for either).
    std::set<std::string> titles;
    size_t pos = 0;
    while (pos < guide.size()) {
        size_t nl = guide.find('\n', pos);
        if (nl == std::string_view::npos) nl = guide.size();
        std::string_view line = guide.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() < 5 || !line.starts_with("**") || !line.ends_with("**")) continue;
        std::string title(line.substr(2, line.size() - 4));
        titles.insert(title);
        for (size_t s = 0, e; s <= title.size(); s = e + 3) {
            e = title.find(" / ", s);
            if (e == std::string::npos) e = title.size();
            titles.insert(title.substr(s, e - s));
        }
    }
    const NodeRegistry& reg = NodeRegistry::instance();
    for (const std::string& type : reg.types()) {
        const NodeInfo* inf = reg.find(type);
        if (!inf || inf->hidden) continue;
        INFO("node: " << inf->displayName);
        CHECK(titles.count(inf->displayName) == 1);
    }
}
