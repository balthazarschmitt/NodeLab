cmake_minimum_required(VERSION 3.24)
# The app version. Bump it (and CHANGELOG.md) when a feature set lands; see CLAUDE.md.
project(NodeLab VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NODELAB_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)
# Portable CMake builds (e.g. WinLibs) ship without CA certificates; reuse Git for Windows' bundle.
set(_git_ca "C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt")
if(NOT CMAKE_TLS_CAINFO AND EXISTS "${_git_ca}")
    set(CMAKE_TLS_CAINFO "${_git_ca}")
endif()
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- dependencies
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.8-docking
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)

FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)

# SOURCE_SUBDIR _none: download only, don't add_subdirectory (no usable CMake in those repos).
FetchContent_Declare(tinyexpr
    GIT_REPOSITORY https://github.com/codeplea/tinyexpr.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)

FetchContent_MakeAvailable(glfw json imgui stb tinyexpr)

find_package(OpenGL REQUIRED)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

add_library(tinyexpr STATIC ${tinyexpr_SOURCE_DIR}/tinyexpr.c)
target_include_directories(tinyexpr PUBLIC ${tinyexpr_SOURCE_DIR})

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

# ---------------------------------------------------------------- version info
# Regenerated on every build so the commit hash is current; the file only changes when the hash does.
set(NODELAB_VERSION_CPP ${CMAKE_BINARY_DIR}/generated/Version.cpp)
add_custom_target(nodelab_version
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_SOURCE_DIR}/cmake/Version.cpp.in
        -DOUT=${NODELAB_VERSION_CPP}
        -DGIT_DIR=${CMAKE_SOURCE_DIR}
        -DVERSION=${PROJECT_VERSION}
        -P ${CMAKE_SOURCE_DIR}/cmake/GitVersion.cmake
    BYPRODUCTS ${NODELAB_VERSION_CPP}
    COMMENT "Stamping version ${PROJECT_VERSION}")

# GUIDE.md is compiled in so Help > Guide works without files next to the exe.
set(NODELAB_GUIDE_CPP ${CMAKE_BINARY_DIR}/generated/Guide.cpp)
add_custom_command(OUTPUT ${NODELAB_GUIDE_CPP}
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/GUIDE.md -DOUT=${NODELAB_GUIDE_CPP} -DNAME=kGuideMarkdown
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedText.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/GUIDE.md ${CMAKE_SOURCE_DIR}/cmake/EmbedText.cmake
    COMMENT "Embedding GUIDE.md")

# Windows can't overwrite a running exe but can rename it; move the old one aside before linking
# so a single build/ folder works while NodeLab is open.
function(nodelab_allow_relink target)
    add_custom_command(TARGET ${target} PRE_LINK
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${target}> -P ${CMAKE_SOURCE_DIR}/cmake/MoveAsideExe.cmake)
endfunction()

# ---------------------------------------------------------------- core library (no GUI)
add_library(nodelab_core STATIC
    ${NODELAB_VERSION_CPP}
    ${NODELAB_GUIDE_CPP}
    src/core/Value.cpp
    src/core/Curve.cpp
    src/core/Ramp.cpp
    src/graph/Graph.cpp
    src/graph/NodeRegistry.cpp
    src/graph/Evaluator.cpp
    src/io/ImageIO.cpp
    src/io/ImageCache.cpp
    src/io/ProjectFile.cpp
    src/nodes/io/IONodes.cpp
    src/nodes/color/ColorNodes.cpp
    src/nodes/math/MathNodes.cpp
    src/nodes/converter/ConverterNodes.cpp
    src/nodes/converter/Expression.cpp
    src/nodes/group/GroupNodes.cpp
    src/nodes/ImageOps.cpp
    src/nodes/filter/FilterNodes.cpp
    src/nodes/transform/TransformNodes.cpp
    src/nodes/matte/MatteNodes.cpp
    src/nodes/texture/TextureNodes.cpp
    src/nodes/utility/UtilityNodes.cpp)
target_include_directories(nodelab_core PUBLIC src)
add_dependencies(nodelab_core nodelab_version)
target_link_libraries(nodelab_core PUBLIC nlohmann_json::nlohmann_json stb tinyexpr)
if(WIN32)
    target_compile_definitions(nodelab_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------- application
add_executable(NodeLab
    src/main.cpp
    src/ui/App.cpp
    src/ui/ImageView.cpp
    src/ui/NodeEditor.cpp
    src/ui/Inspector.cpp
    src/ui/GuideWindow.cpp
    src/ui/FileDialog.cpp
    src/ui/UiScript.cpp
    src/ui/ParamWidgets.cpp)
target_link_libraries(NodeLab PRIVATE nodelab_core imgui)
nodelab_allow_relink(NodeLab)
if(WIN32)
    enable_language(RC)
    configure_file(cmake/NodeLab.rc.in ${CMAKE_BINARY_DIR}/generated/NodeLab.rc @ONLY)
    target_sources(NodeLab PRIVATE ${CMAKE_BINARY_DIR}/generated/NodeLab.rc)
endif()
if(WIN32)
    target_link_libraries(NodeLab PRIVATE comdlg32 ole32)
endif()
if(MINGW)
    target_link_options(NodeLab PRIVATE -static -static-libgcc -static-libstdc++)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(NodeLab PRIVATE -mwindows)
    endif()
endif()

# ---------------------------------------------------------------- tests
if(NODELAB_BUILD_TESTS)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR _none)
    FetchContent_MakeAvailable(doctest)

    enable_testing()
    add_executable(nodelab_tests tests/test_main.cpp tests/test_core.cpp tests/test_nodes.cpp tests/test_groups.cpp tests/test_nodes2.cpp tests/test_version.cpp tests/test_guide.cpp)
    target_include_directories(nodelab_tests PRIVATE ${doctest_SOURCE_DIR})
    target_link_libraries(nodelab_tests PRIVATE nodelab_core)
    nodelab_allow_relink(nodelab_tests)
    if(MINGW)
        target_link_options(nodelab_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME nodelab_tests COMMAND nodelab_tests)
endif()
