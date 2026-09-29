cmake_minimum_required(VERSION 3.24)
project(NodeLab VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NODELAB_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)
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
    GIT_TAG v1.91.8
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)

FetchContent_Declare(imnodes
    GIT_REPOSITORY https://github.com/Nelarius/imnodes.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    GIT_SUBMODULES ""
    SOURCE_SUBDIR _none)

FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)

# SOURCE_SUBDIR _none: download only, don't add_subdirectory (no usable CMake in those repos).
FetchContent_MakeAvailable(glfw json imgui imnodes stb)

find_package(OpenGL REQUIRED)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imnodes_SOURCE_DIR}/imnodes.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imnodes_SOURCE_DIR})
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

add_library(stb INTERFACE)
target_include_directories(stb INTERFACE ${stb_SOURCE_DIR})

# ---------------------------------------------------------------- core library (no GUI)
add_library(nodelab_core STATIC
    src/core/Value.cpp
    src/graph/Graph.cpp
    src/graph/NodeRegistry.cpp
    src/graph/Evaluator.cpp
    src/io/ImageIO.cpp
    src/io/ImageCache.cpp
    src/io/ProjectFile.cpp
    src/nodes/io/IONodes.cpp
    src/nodes/color/ColorNodes.cpp
    src/nodes/math/MathNodes.cpp)
target_include_directories(nodelab_core PUBLIC src)
target_link_libraries(nodelab_core PUBLIC nlohmann_json::nlohmann_json stb)
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
    src/ui/FileDialog.cpp
    src/ui/UiScript.cpp)
target_link_libraries(NodeLab PRIVATE nodelab_core imgui)
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
    add_executable(nodelab_tests tests/test_main.cpp tests/test_core.cpp)
    target_include_directories(nodelab_tests PRIVATE ${doctest_SOURCE_DIR})
    target_link_libraries(nodelab_tests PRIVATE nodelab_core)
    if(MINGW)
        target_link_options(nodelab_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME nodelab_tests COMMAND nodelab_tests)
endif()
