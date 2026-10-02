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
- **Dependencies:** FetchContent pulls glfw, imgui (v1.91.8-docking), nlohmann/json, stb, tinyexpr, zlib-ng,
  LibRaw (0.21.3) and doctest.
  - LibRaw has no CMake build; `CMakeLists.txt` lists its sources (from its `Makefile.dist`). It is
    built with OpenMP, forced on with `LIBRAW_FORCE_OPENMP` because LibRaw disables OpenMP for MinGW.
    `-fopenmp` and the define are PUBLIC because LibRaw's inline allocator differs with OpenMP.
  - `RawDecode.cpp` replaces LibRaw's single-threaded `recover_highlights` (Highlights:
    Reconstruct) with a bit-identical parallel copy run from `post_interpolate_cb`, and reads
    `imgdata.image` through the output curve instead of `dcraw_make_mem_image`. Recheck both
    against LibRaw's code when upgrading it.
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
  - `test_fuzz.cpp` runs every node on 1-pixel-wide images, params at their range ends, NaN and
    infinite pixels, and malformed project params and links. It checks for crashes, hangs and
    non-finite output (except Math and Converter nodes). Its last case loads the example projects
    with random damage anywhere in their JSON.
  - `test_decode_fuzz.cpp` loads damaged JPEG/PNG files and ICC profiles. Set
    `NODELAB_FUZZ_RUNS=5000` for a longer local hunt, and `NODELAB_FUZZ_RAW=<file>` to damage a
    camera RAW too (opt-in, as it needs a real file).
  - `test_guide.cpp` fails if a node has no `**Display Name**` entry in GUIDE.md. Document new
    nodes there (and in the README node table). GUIDE.md is compiled into the exe
    (`cmake/EmbedText.cmake`) and shown by Help > Guide.
- **Node list:** `NodeLab.exe --list-nodes` prints every node with its pins and params.
- **Headless render:** `NodeLab.exe --render project.nlproj out.png` renders at full resolution and
  also writes File Output nodes. The extension picks the format (.png/.jpg/.tif/.exr), and
  `--depth 16` (or 32 for full-float EXR) sets the bit depth. `--timings` prints evaluate and
  save times.
- **Benchmark:** `NodeLab.exe --benchmark project.nlproj [--full] [--runs N]` prints the median ms
  per node. Use it before and after performance work.
  - With `--device gpu`, per-node times are timer queries on queued work and can land on the
    wrong node (one that reads back waits for everything before it). Add `--sync` for each
    node's own time: it waits around every node and doesn't fuse.
- **Screenshot of the UI:** `NodeLab.exe project.nlproj --screenshot shot.png`. Automated runs use
  the CPU device; add `--device gpu` to check the GPU path (evaluation and the viewer's display).
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
src/graph     Node (params, flags, region policy), Graph (links, frames, JSON), Evaluator (cache levels,
              region evaluation, +AsyncEvaluator with drafts and details), NodeRegistry, NodeMenu (Add menu layout), Recipes
              (one-click graph edits such as Add Mask)
src/nodes     one file per family: io, color, math (Mix), converter (+Expression), filter, transform,
              matte, texture, utility, group; filter/Denoise (a-trous wavelets); filter/SpotRemoval
              (spots edited by ViewerOverlay); color/AutoTone (solves Basic's tone sliders by
              running Basic); ImageOps (sampling, box blur, distance transform)
src/io        image load (stb; PngDecode with zlib-ng and JpegDecode, a parallel copy of stb's
              JPEG decoder, both bit-identical to stb), ImageWrite (PNG/JPEG/TIFF/EXR, ICC, parallel zlib-ng and JPEG strips; Tiff.h IFD writer),
              RawDecode (LibRaw), Exif (orientation, export EXIF), Icc (embedded input profiles), Export (Lanczos resize), ImageCache (proxies
              per edge; full-res decoded on demand, scaled levels kept for regions; RAW proxies from a
              half-size decode), project files, Library (folder listing, sidecars `photo.ext.nlproj`
              with rating/flag/thumbnail in `ui.library`, default graph, paste edit, thumbnails),
              Presets (`.nlpreset` node snippets in %APPDATA%\NodeLab\presets)
src/ui        App (docking, viewers, undo, groups nav, eyedropper), NodeEditor (custom canvas), Inspector,
              GuideWindow (renders the embedded GUIDE.md), Eyedropper (pick state), Theme (preset
              and custom colour themes: NodeEditor and ImageView draw themeable colours with
              theme::col), Preferences window and layout presets (in App),
              ParamWidgets (curve/ramp editors), ImageView, DisplayWorker (view transform and
              histograms off the UI thread), LibraryPanel (filmstrip and Grid view, culling keys,
              thumbnail worker), FileDialog (Win32), UiScript
src/gpu       Device (hidden GL 4.3 context sharing textures with the UI, texture pool, programs, timer queries, PBO downloads),
              GL (loader), PointOp (per-pixel nodes as GLSL bodies, fused into chains), Blur,
              Reduce (exact percentiles by radix select), Display (viewer bytes and histogram)
```

## Conventions

- **Adding a node:**
  1. Write a class with `NODELAB_NODE({type, name, category, inputs, outputs, params})` and `evaluate()`.
  2. Register it in the family's `register*Nodes()`.
  3. Add a `**Display Name**` entry to GUIDE.md (enforced by `test_guide.cpp`).
  4. List it in the Add menu's layout in `graph/NodeMenu.cpp` (otherwise it lands at the end of
     its category's menu). `test_node_menu.cpp` keeps each menu at 20 nodes or fewer.

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
- **Cache levels:** the Evaluator caches the Preview, a Draft (half the proxy edge, while dragging
  slow graphs) and a Region level separately. `AsyncEvaluator` trims the draft and region levels
  to a memory budget. Exports set `releaseIntermediates`.
- **Region evaluation** (zoomed-in detail, `Evaluator::evaluateRegion`) runs each node on part of
  its image, with `ctx.roi` set (the window and the node's full size).
  - **Padding:** override `roiPadding()` with how far the node reads around each pixel. Use 0 for
    per-pixel nodes, and `kRoiWhole` (the default outside Color/Mix/Converter) if it needs the
    whole image.
  - **Coordinates:** anything that depends on position or image size (generators, Relative
    sizes, longEdge) must use `nodeutil::frameOf(ctx, w, h)` for the global origin and full size,
    never the buffer's.
  - **Mapping nodes:** nodes that move pixels (Crop, Flip) implement `roiMap`, and
    `roiOutputSize` if they change the size.
  - **Global statistics:** record them through `ctx.statsOut` in a preview run, and read
    `ctx.previewStats` in a region (Normalize, Basic's Dehaze).
  - **Test:** `test_roi.cpp` checks that every node's region matches the same part of its whole
    image. Add a variant for any new size- or position-dependent param.
- **GPU nodes** (Blender's compositor Device: GPU): a node opts in with `gpuSupported` and
  `evaluateGpu`, usually a `gpu::PointOp` GLSL body that mirrors its C++ loop.
  - Values on the device are `GpuImagePtr`/`GpuChannelPtr`. The evaluator converts inputs between
    devices and caches the copies. GPU runs happen at the Preview, Draft and Region levels;
    File > Export uses the device at Full precision (holding it only while evaluating);
    `--render` uses the CPU unless `--device gpu`.
  - **Regions on the GPU:** `evaluateGpu` must honour `ctx.roi` as `evaluate` does (`frameOf` for
    positions and sizes, `previewStats` for global statistics, Crop's window mapping).
    `test_roi.cpp`'s GPU case checks every node.
  - A `gpu::Error` falls back to the CPU. `test_gpu.cpp` compares every GPU node with its CPU
    version (it skips when there's no GPU). Automated UI runs use the CPU device unless given
    `--device gpu`.
  - **Viewer:** `gpu::display` (`src/gpu/Display.cpp`) mirrors `colormgmt::viewTransform`,
    `displayBytes` and `Histogram::compute`; change them together. It writes an RGBA8 texture,
    because reading back a buffer a shader wrote is about 10x slower than a texture through
    the pack buffer.
    - Viewer results evaluated on the GPU stay there (`AsyncEvaluator::Result::gpuImages`, with a
      null image). `DisplayWorker::download` fetches the pixels only for the eyedropper, masks
      and the CPU fallback.
    - The device context shares textures with the UI's (`gpu::init(why, window)`), so viewers
      draw the RGBA8 texture itself (`GLTexture::showDevice`), with no readback or upload.
      `GLTexture::endFrame` returns a replaced texture to the pool a few frames later, once the
      draws that read it are done. Without sharing, viewers get bytes as before.
  - **Fusion:** `runPoint` doesn't dispatch. Its outputs are *pending* (`GpuValue::pending`), and a
    later point op of the same size compiles the pending stage into its own shader. Each stage's
    names are namespaced with `#define`/`#undef`, so a body and its `functions` use the plain
    names (`P`, `img0`, `has1`, its consts and helpers). Anything else that needs the pixels calls
    `texture()`, never `tex` directly. The evaluator materializes a pending input whose node has
    several wires, so nothing is computed twice.
  - GLSL division is approximate and drivers fold NaN checks: see the `c_*` helpers in
    `Expression.cpp` where exact results matter.
    - Values that feed `floor()` or a comparison at pixel boundaries (texture coordinates,
      cells) are computed on the CPU per row or column and read with `lutAt(i)`; see
      `TextureBase::runGpu`.
  - **Neighbourhood nodes:** list the pins they read at other pixels in `PointOp::gather`
    (`fetch<i>`, `bilinear<i>`, `size<i>`).
    - `runPass` chains the passes; mark sums and other intermediates `full`.
    - `gpu::boxBlur` blurs a texture, and `gpu::select` gives exact ranks (percentiles).
    - Basic's `guidedGpu`/`sumGpu` show a guided filter and a sum read back.
- **Preferences:** `App::loadPreferences`/`savePreferences` (`%APPDATA%\NodeLab\preferences.json`).
  Automated runs neither load nor save them, never auto save, and keep the Inspector as a docked panel, so their
  scripts' coordinates hold.
- **Image buffers:** `Image::px` uses `UninitAllocator`, so `Image(w, h)` zeroes and copies
  across threads. Big scratch buffers that are written before they're read can use it too
  (Denoise).
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
