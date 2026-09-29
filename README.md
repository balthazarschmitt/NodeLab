# NodeLab

Node-based image manipulation. Import a photo, wire color data between nodes, and see the original
(left) and the result (right) update live. Aimed at channel mixing, IR/UV camera emulation, and
glitch/experimental looks.

## Build (Windows)

Requires CMake ≥ 3.24 and a C++20 compiler. CLion's bundled MinGW toolchain works out of the box:
open the folder in CLion, pick the `NodeLab` target, Run. Dependencies (GLFW, Dear ImGui, imnodes,
nlohmann/json, stb, doctest) are downloaded by CMake on first configure.

Command line:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
build\NodeLab.exe
```

**Portable toolchain (no install):** a WinLibs GCC 16 + CMake + Ninja bundle is unpacked in
`.toolchain/` (gitignored). WinLibs' CMake ships without CA certificates, so pass Git's bundle on
first configure:

```
set PATH=%CD%\.toolchain\bin;%PATH%
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TLS_CAINFO="C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt"
```

The Release exe is statically linked, so no extra DLLs are needed to run it.

Headless render: `NodeLab.exe --render project.nlproj out.png`
Screenshot of the UI (debug aid): `NodeLab.exe project.nlproj --screenshot shot.png`
Scripted UI test (feeds input straight to ImGui, ignores the real mouse; see `src/ui/UiScript.h`):
`NodeLab.exe examples\demo.nlproj --script tests\ui\fanout_addnode.txt`

Try `examples/demo.nlproj`: the red channel drives saturation, so only red things stay colorful.

## Using it

| Action | How |
|---|---|
| Add a node | Right-click the canvas (type to search) |
| Connect | Drag from an output pin to an input pin |
| Disconnect | Drag a wire off its input pin |
| Delete | Select, then press Delete |
| Preview any node | Ctrl+click it (again to clear) |
| Zoom / pan images | Mouse wheel / drag; double-click resets. Both panes stay in sync |
| Import image | File > Import Image, or drop a file on the window |

**Wire types**
- **Image** (amber, square pins): full RGBA.
- **Channel** (gray): one grayscale plane.
- **Number** (blue): a single value.

Conversions are automatic:
- An Image plugged into a Channel input becomes its luminance.
- A Number plugged into a Channel input becomes a flat value.

Most sliders on a node are Channel inputs too. For example, Split RGB → R into Saturation → Amount
makes the saturation follow the red channel pixel by pixel.

Projects (`.nlproj`) are JSON. Image paths are stored relative to the project file.

## Layout

```
src/core    Image/Channel/Value types, conversions, parallelFor
src/graph   Node model, Graph (links, cycle check, JSON), Evaluator (cached, background thread)
src/nodes   Node implementations by family (io, color, math, ...)
src/io      Image load/save, source image cache, project files
src/ui      App window, node editor (imnodes), inspector, image views, file dialogs
tests       doctest unit tests
```

Adding a node: write a class with a `NODELAB_NODE({...})` descriptor and `evaluate()`, then
`r.add<YourNode>()` in its family's register function.
