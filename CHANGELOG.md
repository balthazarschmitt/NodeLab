# Changelog

NodeLab uses [semantic versioning](https://semver.org). While it is 0.x, minor versions add
features and patch versions fix bugs. Each release is tagged `vX.Y.Z` in git.

## Unreleased

## 0.6.0 (2026-09-30)

- **Faster processing.** Typical graphs re-render about 3–5× faster. The infrared preset takes
  about 206 ms at preview size (was about 640 ms) and 570 ms at full resolution (was about 2.9 s):
  - Expression nodes compile to bytecode that runs over runs of pixels, sharing repeated
    sub-expressions. The results are bit-identical.
  - Worker threads start once and are reused, instead of being created for every loop.
  - Blur reads memory in cache-friendly blocks. Blurring a Channel keeps it a single channel
    instead of converting it to RGBA.
  - A long render is cancelled mid-node when you change something. Nodes already finished stay
    cached, so the new render only redoes what changed.
- **View > Node Timings:** each node shows how long it took to evaluate, as in Blender's
  compositor. Slow nodes (50 ms or more) are highlighted.
- `NodeLab.exe --benchmark project.nlproj [--full] [--runs N]` prints per-node and total times.
- **Infrared foliage** example (`examples/infrared_foliage.nlproj`) and Guide recipe: lilac-white
  glowing trees, a dark maroon sky and pink clouds. It was fitted against a real infrared/colour
  photo pair, handles sky seen through needles (no halos), and works at any resolution or exposure.
- **Blur > Relative** (Blender's option): Factor X/Y as a percentage of the image size, with
  Aspect Correction, so one setting fits any resolution.
- **Normalize > Low % / High %:** percentile range (auto-exposure). The 0 / 100 defaults keep
  the old min/max behaviour.
- Node params can now show only while another param is set (Blur's Factor X/Y appear only with
  Relative, as in Blender).
- Fix: **Combine RGB** clamped its inputs and output to 0..1. It is now unclamped like Blender's,
  so it can pack masks and values outside 0..1.
- Fix: an Image Input that fails to load now names the file in the error.

## 0.5.0 (2026-09-29)

- **Basic** node: Lightroom's Basic panel in one node (temperature, tint, exposure, contrast,
  highlights, shadows, whites, blacks, texture, clarity, dehaze, vibrance, saturation), with the
  sliders grouped in the Inspector.
- **Color Mixer** (8-band HSL with Hue / Saturation / Luminance tabs) and **Color Grading**
  (colour wheels for shadows, midtones, highlights and global, with Blending and Balance).
- **Masks:** Radial Gradient, Linear Gradient and Brush Mask, edited directly on the Result
  panel with handles or by painting (Alt erases, `[` `]` size). A selected mask is tinted red over
  the image (O).
- **Crop** gains Angle (straighten), Aspect presets and Constrain to Image. While selected, the
  Result shows the whole frame with a draggable crop rectangle; drag outside it to straighten.
- **Lens Correction:** distortion, red/cyan and blue/yellow fringing, and vignetting with
  midpoint.
- **Histogram** (H) and **clipping warnings** (J) on the Result panel.
- Nodes can be "compact" (settings only in the Inspector) so large nodes like Basic stay small in
  the graph. Inspector sliders with wide ranges show one decimal.
- **Export window** (File > Export, Ctrl+E): exports render on a background thread with progress
  and Cancel instead of freezing the app. Choose PNG or JPEG (with quality) and an optional
  downscale (long edge or percent). PNG saving is faster: lighter compression, and no alpha
  channel when the image is opaque. File > Write File Outputs also runs in the background.
- **Batch export:** run a list of photos (files, a folder, or dropped images) through the node tree
  into an output folder as `<name>_edit.png/.jpg`; also `NodeLab.exe --batch`.
- **Swap** (Shift+S, node right-click → Swap...): changes a node's type in place, keeping its wires
  where pins match and settings with the same name, as in Blender.
- **Auto spacing:** added, pasted, swapped and spliced nodes push the nodes they overlap out of
  the way. **Arrange** (Shift+P, Edit > Arrange Nodes) lays the selection or the whole graph out in
  columns.
- The Original pane is titled just "Original", and the Result pane no longer shows an "Output"
  label.
- `examples/infrared.nlproj`: infrared false-colour look (lilac-white foliage, maroon sky) from two
  Image Expression nodes plus Glare; swap the Image Input for your own photo.
- Curves editor (Curves, Hue Correct, Float Curve) fits the Inspector: it now stretches to the
  panel's width and fits its height instead of hanging off the bottom, the help tooltip no
  longer covers the curve while dragging, and each node remembers its own selected channel.

## 0.4.0 (2026-09-29)

- Move nodes between frames: node right-click → Move to Frame, frame right-click → Move Selected
  Nodes Here, Alt+P removes from frame.
- **Guide:** GUIDE.md explains every node, colour space and data type, with recipes. It is built
  into the app as Help > Guide (F1), with a contents tree and search; F1 and the Inspector's Guide
  button open it at the selected node.
- **Eyedropper** on every colour setting: click a pixel or drag a rectangle (area average) on the
  Original, Result or a viewer.
- **Intermediate results:** node right-click → Open in New Viewer; viewers get a node drop-down
  and a Sync view option.
- Shift+A opens the add-node menu at the mouse, as in Blender.
- `--list-nodes` prints every node with its pins and settings.

## 0.3.0 (2026-09-29)

- **Blender node set:**
  - Filter: Blur, Directional and Bilateral Blur, Filter kernels, Dilate/Erode, Kuwahara, Pixelate,
    Posterize, Glare, Sun Beams.
  - Transform: Transform, Flip, Crop, Lens Distortion, Displace, Map UV, Corner Pin.
  - Matte: Box and Ellipse masks; Channel, Luminance, Difference, Distance and Chroma keys; Color Spill;
    Double Edge Mask.
  - Texture: Noise, Voronoi, Gradient, Wave, Checker, White Noise.
  - Converter: Wavelength, Blackbody, Normalize, Float Curve, Set Alpha.
  - Mix: Alpha Over.
  - Utility: Reroute, Switch, Split, Image Info, File Output.
  - Color: Hue Correct, Color Balance, Tone Map, Convert Colorspace, YCbCr/YUV/HSL split and combine.
- **Add-node menu:** a search box sits at the top.
- **Blender editing shortcuts:**
  - Delete with reconnect, and Alt+drag to pull a node out of its chain.
  - Mute, collapse, rename, copy/paste, grab (G), duplicate and grab (Shift+D).
  - Make links, select linked, framing.
  - Knife cut and reroute insertion; auto-offset when splicing.
- **Versioning:**
  - The version shows in the window title, the Help menu, `--version`, and the exe's properties.
  - Projects record the app version that saved them.
- **Single `build/` folder:** it can be rebuilt while NodeLab is running.

## 0.2.0

- **M2 nodes:** colour adjustments, colour spaces (HSV, Lab), Color Ramp, keys, Math, Map Range,
  Expression nodes.
- **Custom node canvas:** pan/zoom, splice-on-wire, wire-to-add-menu, duplicate, undo/redo.
- **Panels:** dockable, rearrangeable, plus extra viewers that pin any node.
- **Organisation:** node groups (nested) and frames.

## 0.1.0

- **M1 skeleton:**
  - Typed Image/Channel/Number wires, evaluator with cache and background evaluation.
  - Original/result views, save and load of `.nlproj` projects, core colour nodes.
