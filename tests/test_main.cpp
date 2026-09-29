#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "graph/NodeRegistry.h"

int main(int argc, char** argv) {
    registerAllNodes();
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
