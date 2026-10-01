# NodeLab

Node-based image manipulation. Import a photo, wire color data between nodes, and see the original
(left) and the result (right) update live. Aimed at channel mixing, IR/UV camera emulation, and
glitch/experimental looks.

## Build (Windows)

Requires CMake ≥ 3.24 and a C++20 compiler. CLion's bundled MinGW toolchain works out of the box:
open the folder in CLion, pick the `NodeLab` target, Run. Dependencies (GLFW, Dear ImGui, imnodes,
nlohmann/json, stb, tinyexpr, LibRaw, zlib, doctest) are downloaded by CMake on first configure.

LibRaw (camera RAW decoding) is used under its CDDL 1.0 licence option
(https://github.com/LibRaw/LibRaw/blob/master/LICENSE.CDDL). It is built with OpenMP and linked
statically, so the exe still needs only Windows system DLLs.

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

Always build into the one `build/` folder. Rebuilding works while `build\NodeLab.exe` is open: the
running copy is renamed to `NodeLab.old-*.exe` and deleted by a later build once it has closed.

Version: `NodeLab.exe --version` (also in the window title, Help menu and the exe's Properties).
See [CHANGELOG.md](CHANGELOG.md).

Headless render: `NodeLab.exe --render project.nlproj out.png [--depth 16]` (the extension picks
PNG, JPEG, TIFF or OpenEXR; `--depth 32` for full-float EXR)
Headless batch: `NodeLab.exe --batch project.nlproj outDir [--png|--jpg|--tif|--exr] [--depth N] a.jpg b.jpg ...`
(each source goes into the project's first Image Input and is saved as `outDir\<name>_edit.<ext>`)
Benchmark: `NodeLab.exe --benchmark project.nlproj [--full] [--runs N]` (median ms per node)
Screenshot of the UI (debug aid): `NodeLab.exe project.nlproj --screenshot shot.png`
Scripted UI test (feeds input straight to ImGui, ignores the real mouse; see `src/ui/UiScript.h`):
`NodeLab.exe examples\demo.nlproj --script tests\ui\fanout_addnode.txt`

Try `examples/demo.nlproj`: the red channel drives saturation, so only red things stay colorful.
`examples/infrared_foliage.nlproj` turns a colour photo into lilac-white infrared foliage under a
dark sky (see Help > Guide > Recipes).

## Using it

| Action | How |
|---|---|
| Add a node | Right-click the canvas (or Shift+A) and start typing to search (Up/Down + Enter), or browse the categories |
| Connect | Drag from a pin to another pin (or onto a node body) |
| Add a connected node | Drag a wire into empty space, pick a node |
| Splice into a wire | Drag an unconnected node onto a wire and release (drag off to cancel); downstream nodes shift to make room |
| Swap a node's type | Shift+S (or right-click → Swap...) and pick the new type; wires and matching settings carry over |
| Auto spacing | New, pasted, swapped and spliced nodes push overlapping nodes out of the way |
| Arrange | Shift+P (Edit > Arrange Nodes) lays the selection (2+ nodes), or the whole graph, out in tidy columns |
| Disconnect | Drag a wire off its input pin |
| Pan / zoom graph | Drag empty space (or middle-drag) / mouse wheel |
| Select | Click, Shift+click to add, Shift+drag a box, Ctrl+A all |
| Duplicate / delete | Ctrl+D / Delete (also on the node's right-click menu) |
| Undo / redo | Ctrl+Z / Ctrl+Y |
| Delete | Delete / X reconnects the wires around the node; Alt+Delete deletes without reconnecting |
| Copy / paste | Ctrl+C / Ctrl+V (pastes at the mouse, keeps wires between copied nodes) |
| Move / duplicate-and-move | G / Shift+D, then click to place (right-click or Esc cancels) |
| Pull a node out of a chain | Alt+drag it |
| Mute / collapse / rename | M / H / F2 |
| Make links | F connects the selected nodes left to right |
| Select linked | L upstream, Shift+L downstream |
| Cut wires / add reroutes | Ctrl+right-drag / Shift+right-drag across wires |
| Preview another output | Ctrl+Shift+click a node cycles through its outputs |
| Frame all / selected | Home / . |
| Edit a value | Drag the field sideways (Shift = fine), or click it to type |
| Preview any node | Ctrl+click it (again to clear) |
| Group / ungroup | Ctrl+G / Ctrl+Alt+G; Tab (or double-click) enters a group, Tab leaves |
| Group pins | Select the group (or its Group Input/Output inside) and edit in the Inspector |
| Frame | Ctrl+J around the selection; drag its title to move it with its nodes, corner to resize, double-click to rename, right-click for color |
| Move nodes between frames | right-click a node → Move to Frame, or select nodes and right-click a frame title → Move Selected Nodes Here; Alt+P removes from frame |
| Panels | Drag a panel's tab to dock it elsewhere, or out of the window; View > Reset Layout |
| Extra viewers | Right-click a node → Open in New Viewer (or View > New Viewer) to watch an intermediate result; the viewer's drop-down switches node, "Sync view" pans with the other panes |
| Eyedropper | "Pick" next to a colour setting (or Pick from Image in the node's colour popup), then click a pixel or drag a rectangle on any image panel for the area's average; right-click / Esc cancels |
| Guide | Help > Guide or F1 (opens at the selected node's entry); also the Inspector's Guide button |
| Zoom / pan images | Mouse wheel / drag; double-click resets. Both panes stay in sync |
| Colour management | Color menu: View Transform (Standard, AgX, Raw), Look, view Exposure and Gamma, as in Blender's Render Properties. New projects are scene-linear; Convert Project to Scene-Linear upgrades a legacy one |
| Histogram / clipping | Result toolbar, or H / J with the mouse over the Result: RGB histogram, and clipped highlights in red and crushed shadows in blue |
| On-image controls | Select a Crop, gradient, shape or Brush Mask node and edit it on the Result: drag handles; Crop shows the whole frame (drag outside to straighten); Brush paints, Alt erases, `[` `]` size; O toggles the red mask overlay |
| Import image | File > Import Image, or drop a file on the window |
| Export | File > Export (Ctrl+E) opens the Export window: renders in the background with a progress bar and Cancel. Format (PNG/TIFF 8 or 16 bit, JPEG + quality with EXIF, OpenEXR half/full float scene-linear), size (original, long edge, percent; Lanczos in linear light) |
| Batch | Export window → Batch: add files or a folder (or drop them on the window), pick the Image Input they feed and an output folder; each result is saved as `<name><suffix>` |

**Wire types**
- **Image** (amber, square pins): full RGBA.
- **Channel** (gray): one grayscale plane.
- **Number** (blue): a single value.

Conversions are automatic:
- An Image plugged into a Channel input becomes its luminance.
- A Number plugged into a Channel input becomes a flat value.

Most sliders on a node are Channel inputs too. For example, Split RGB → R into Saturation → Amount
makes the saturation follow the red channel pixel by pixel.

Projects (`.nlproj`) are JSON. Image paths are stored relative to the project file. The panel
layout is saved per user in `%APPDATA%\NodeLab\layout.ini`.

## Nodes

| Category | Nodes |
|---|---|
| Input / Output | Image Input, Output, Number |
| Color | Basic (Lightroom's exposure, highlights/shadows, whites/blacks, texture, clarity, dehaze, vibrance), Color Mixer (8-band HSL), Color Grading (shadow/midtone/highlight/global wheels), Brightness / Contrast, Saturation, Hue Shift, Hue Correct (per-hue H/S/V curves), Exposure, Gamma, Levels, Curves, Color Balance (Lift/Gamma/Gain, ASC CDL), Tone Map, Convert Colorspace, Invert, Luminance, Split/Combine RGB, HSV, HSL, Lab, YCbCr, YUV |
| Mix | Mix, Blend (19 modes), Alpha Over |
| Converter | Color Ramp, Color Key, Map Range, Math (21 ops), Clamp, Threshold, Normalize (min/max or percentiles), Float Curve, Set Alpha, Wavelength (nm to color), Blackbody (Kelvin to color), Expression, Image Expression |
| Filter | Blur (pixels or Relative %), Directional Blur (+spin/zoom), Bilateral Blur, Filter (Soften, Sharpen, Laplace, Sobel, Prewitt, Kirsch, Shadow), Dilate / Erode, Kuwahara, Pixelate, Posterize, Glare (Fog Glow, Streaks, Simple Star), Sun Beams |
| Transform | Transform, Flip, Crop (straighten, aspect presets, on-image frame), Lens Correction (distortion, fringing, vignetting), Lens Distortion (with chromatic dispersion), Displace, Map UV, Corner Pin |
| Matte | Box Mask, Ellipse Mask, Radial Gradient, Linear Gradient, Brush Mask (painted on the Result), Channel Key, Luminance Key, Difference Key, Distance Key, Chroma Key, Color Spill, Double Edge Mask |
| Texture | Noise, Voronoi, Gradient, Wave, Checker, White Noise |
| Utility | Reroute, Switch, Split (compare), Image Info, File Output (PNG/JPEG/TIFF/OpenEXR; written on single Export / File > Write File Outputs / --render) |
| Group | Groups (Ctrl+G) with Group Input / Group Output inside |

Sizes in pixels (blur radius, offsets, glare size) refer to the full-resolution image; the preview
scales them so it matches the export. Textures use image-relative coordinates for the same reason.

Color adjustment outputs are clamped to 0..1 and their parameters to their ranges. Math and
converter nodes are unclamped (enable Clamp where offered) so intermediate values can go
negative or above 1.

Expression variables: `r g b a` (Image input), `in1 in2`, `x y` (pixel), `u v` (0..1), `w h`;
functions include `sin cos pow sqrt abs floor ceil log exp atan2 min max clamp mix step smoothstep fract`.

## Layout

```
src/core    Image/Channel/Value types, conversions, parallelFor
src/graph   Node model, Graph (links, cycle check, JSON), Evaluator (cached, background thread)
src/nodes   Node implementations by family (io, color, math, converter, group)
src/io      Image load/save, source image cache, project files
src/ui      App window, node editor (imnodes), inspector, image views, file dialogs
tests       doctest unit tests
```

Adding a node: write a class with a `NODELAB_NODE({...})` descriptor and `evaluate()`, then
`r.add<YourNode>()` in its family's register function.
