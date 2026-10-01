# NodeLab

Node-based image editor (C++20, Dear ImGui docking branch + GLFW/OpenGL 3, CPU float pipeline).
The user thinks in Blender's node editor and compositor; match Blender's behavior and names when
adding features.

## Build and run

The toolchain is portable and lives in `.toolchain/` (WinLibs GCC 16, CMake, Ninja), which is gitignored.
Nothing is installed system-wide.

```
set PATH=C:\Projects\NodeLab\.toolchain\bin;%PATH%
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\nodelab_tests.exe
```

- **One build directory:** always build into `build/`. The user wants no other build folders, so don't
  create `build-dev/`, `build-test/` or similar.
  - The user often has `build\NodeLab.exe` running. A pre-link step (`cmake/MoveAsideExe.cmake`)
    renames the running exe to `NodeLab.old-<timestamp>.exe`, because Windows allows renaming a
    running exe but not overwriting it. The next build deletes old copies that are no longer running.
  - Never kill the user's NodeLab process.
- **Dependencies:** FetchContent pulls glfw, imgui (v1.91.8-docking), nlohmann/json, stb, tinyexpr,
  LibRaw (0.21.3) and doctest.
  - LibRaw has no CMake build; `CMakeLists.txt` lists its sources (from its `Makefile.dist`). It is
    built with OpenMP, forced on with `LIBRAW_FORCE_OPENMP` because LibRaw disables OpenMP for MinGW.
    `-fopenmp` and the define are PUBLIC because LibRaw's inline allocator differs with OpenMP.
  - If a reconfigure fails while updating a `GIT_TAG master` dependency (stb, tinyexpr) offline,
    pass `-DFETCHCONTENT_UPDATES_DISCONNECTED=ON`.
  - `CMakeLists.txt` defaults `CMAKE_TLS_CAINFO` to Git for Windows' CA bundle, because WinLibs'
    CMake has none.
- **Release linking:** the Release exe links statically (`-static`, `-mwindows`). It needs only
  Windows system DLLs and OpenGL 3.0.

## Versioning

- **App version:** `project(NodeLab VERSION x.y.z)` in `CMakeLists.txt` is the single source.
  - It reaches the code through `core/Version.h` (`kNodeLabVersion`, `kNodeLabCommit`, `versionString()`).
  - `cmake/GitVersion.cmake` stamps the git hash at build time, with `-dirty` for uncommitted changes.
  - It appears in the window title, the Help menu, `NodeLab.exe --version`, and the exe's
    Properties > Details (`cmake/NodeLab.rc.in`).
- **Bumping (semver while 0.x):**
  - Minor (0.3.0 → 0.4.0) when a feature set lands; patch (0.3.0 → 0.3.1) for fixes only.
  - Bump when the user asks to commit a finished feature set, not on every commit.
  - Update `CHANGELOG.md` under the new version, commit, then tag it: `git tag -a v0.4.0 -m "NodeLab 0.4.0"`.
  - Add changes that aren't released yet under `## Unreleased` in `CHANGELOG.md`.
- **Project file format:** `.nlproj` has `version` (the format, `kProjectVersion` in
  `io/ProjectFile.h`) and `appVersion` (the app that saved it).
  - New node types or params don't need a format bump, because unknown params fall back to defaults.
  - Bump `kProjectVersion` only for changes older builds would misread, and make `loadProject`
    upgrade the old layout. Never break loading of older projects.

## Testing

- **Unit tests:** `nodelab_tests.exe` (doctest), in `tests/test_*.cpp`.
  - `test_nodes2.cpp` runs every registered node with image and channel inputs on every pin.
  - New nodes are covered automatically, but add behaviour checks for anything non-trivial.
  - `test_guide.cpp` fails if a node has no `**Display Name**` entry in GUIDE.md. Document new
    nodes there (and in the README node table). GUIDE.md is compiled into the exe
    (`cmake/EmbedText.cmake`) and shown by Help > Guide.
- **Node list:** `NodeLab.exe --list-nodes` prints every node with its pins and params.
- **Headless render:** `NodeLab.exe --render project.nlproj out.png` renders at full resolution and
  also writes File Output nodes.
- **Benchmark:** `NodeLab.exe --benchmark project.nlproj [--full] [--runs N]` prints the median ms
  per node. Use it before and after performance work.
- **Screenshot of the UI:** `NodeLab.exe project.nlproj --screenshot shot.png`.
- **Scripted UI tests:** `NodeLab.exe tests\ui\interact.nlproj --script tests\ui\<name>.txt`.
  - Script commands are documented in `src/ui/UiScript.h`.
  - Input goes straight into ImGui and the real mouse is ignored.
  - Scripts save screenshots with `shot build/smoke/<file>.png`; read them to verify.
  - Coordinates assume the automated 1600x900 window, the default docked layout, and
    `interact.nlproj`'s fixed graph view. The canvas origin is at (439, 72) at zoom 1.
- **Don't drive the user's desktop.** Faking OS input (PostMessage, SendInput) or grabbing screen
  pixels breaks because the real mouse interferes, and it can capture the user's other windows.
  Use `--script` / `--screenshot`.

## Layout

```
src/core      Image/Channel/Value types + conversions, ColorMath, ColorScience (Oklab, CAT16 white
              balance, gamut compression), Curve, Ramp, Noise, Parallel
src/graph     Node (params, flags), Graph (links, frames, JSON), Evaluator (+AsyncEvaluator), NodeRegistry
src/nodes     one file per family: io, color, math (Mix), converter (+Expression), filter, transform,
              matte, texture, utility, group; ImageOps (sampling, box blur, distance transform)
src/io        image load/save (stb), RawDecode (LibRaw), Exif (JPEG orientation), ImageCache (proxy only;
              full-res decoded on demand; RAW proxies from a half-size decode), project files
src/ui        App (docking, viewers, undo, groups nav, eyedropper), NodeEditor (custom canvas), Inspector,
              GuideWindow (renders the embedded GUIDE.md), Eyedropper (pick state),
              ParamWidgets (curve/ramp editors), ImageView, FileDialog (Win32), UiScript
```

## Conventions

- **Adding a node:**
  1. Write a class with `NODELAB_NODE({type, name, category, inputs, outputs, params})` and `evaluate()`.
  2. Register it in the family's `register*Nodes()`.
  3. Add a `**Display Name**` entry to GUIDE.md (enforced by `test_guide.cpp`).

  The `type` string is saved in projects, so never rename one.
- **Wires:** Image (RGBA float), Channel (float plane; `constant` = sizeless), or Number.
  - Conversions happen implicitly (`toImage`, `toChannel`).
  - An input pin with `fallbackParam` shows that param's slider when unconnected.
  - An empty wire also falls back to the slider value.
- **Colour management (Blender model):** the root `Graph::colorManagement` holds the working space
  and the view settings.
  - **Scene-linear** projects (new ones) decode sRGB images to linear light (Image Input's Color
    Space) and apply the view transform (`colormgmt::displayImage`) only in viewers and at export.
  - **Legacy** projects (no `colorManagement` block) work on sRGB-encoded values and must render
    byte-identically. Check `ctx.linear()` in any node whose maths depends on the encoding.
  - `ParamDesc::Color` values are stored linear in linear projects; pickers convert with
    `ui/ColorDisplay.h`. `ParamDesc::ColorGamma` (multipliers such as lift/gain) is shown as stored.
- **Clamping:**
  - Colour nodes clamp outputs with `clampColor(ctx.linear(), v)`: 0..1 in legacy projects, only
    negatives in linear ones. Curves, Invert, Posterize, alpha and mattes stay 0..1.
  - Math and converter nodes are unclamped, with a Clamp option where it makes sense.
  - Params clamp to `hardMin`/`hardMax`. `ParamDesc::Float` sets hard equal to soft;
    `FloatFree` is unbounded.
  - Sample channels that drive params with `paramSampler` (it applies the param's range).
- **Resolution independence:** sizes in pixels (blur radius, offsets) are full-resolution pixels,
  multiplied by `ctx.scale` so the proxy preview matches the export. Textures use image-relative
  coordinates.
- **Evaluation cache key:** `Evaluator::ensure` keys on type, params, `signatureExtra()`, the muted
  flag, working size/scale, and upstream signatures. Node state that affects output must feed into it.
- **Node state:** `Node::muted`, `collapsed` and `label` persist in JSON. `Graph::cloneNode` copies them.
- **Groups:** a `GroupNode` owns an inner `Graph`, and its interface pins are pushed to the inner
  Group Input/Output nodes by `syncInner()`. Use `GroupNode::removePin`/`movePin`/`setPinType`, which
  keep links valid on both sides.
- **Undo:** whole-graph JSON snapshots. They commit when no gesture is active (`NodeEditor::interacting()`).
  - Graph edits made from the UI must set `docChanged`/`evalChanged`, or call `App::markChanged`.
- **Style:** match the surrounding code, with comments explaining *why*. Keep the README's node table
  and controls table up to date when adding features.

## ImGui gotchas (learned the hard way)

- **The canvas background** is an `InvisibleButton` with `SetNextItemAllowOverlap()`. Node widgets
  are submitted after it and win hover.
- **Active widgets:** a widget being dragged must keep being submitted every frame (see
  `activeNode_` / `editing_`), even when the mouse leaves its node, or ImGui drops the active item.
- **Popups:** an ID must be opened and begun in the same ID-stack scope.
- **Tab** is used for entering and exiting groups, so `NavEnableKeyboard` stays off.

## Tooling gotchas

- In the Bash tool, backslash escapes in heredocs get mangled (`\n` became a real newline in C++
  string literals). For edits containing backslashes, write a Python patch script with the Write tool
  and run it, or use the Edit tool.
- PowerShell treats `Move` as the `Move-Item` alias, so don't name helper functions that.

## Git

- Branch `main`, no remote.
- Commit when the user asks, with descriptive messages ending with the Co-Authored-By trailer.
