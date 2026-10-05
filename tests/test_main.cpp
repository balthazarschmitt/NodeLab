#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>

#include "graph/NodeRegistry.h"
#include "io/Paths.h"
#include "ml/Models.h"

int main(int argc, char** argv) {
    registerAllNodes();
    // Every node runs many times in the tests: the AI masks mustn't find the user's installed
    // models (a run takes up to a minute), unless a test run asks for them.
    if (!std::getenv("NODELAB_ML_REAL")) {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "nodelab_tests_models";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        ml::setFolder(pathToU8(dir));
    }
    doctest::Context ctx(argc, argv);
    return ctx.run();
}
