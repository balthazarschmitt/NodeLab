# Changelog

NodeLab uses [semantic versioning](https://semver.org). While it is 0.x, minor versions add
features and patch versions fix bugs. Each release is tagged `vX.Y.Z` in git.

## Unreleased

- **Faster image loading:** a 24 MP image opens 2-3x faster, with exactly the same pixels.
  - JPEG decodes on every core (about 330 ms to 120 ms). Restart intervals decode in parallel,
    and upsampling and colour conversion run per row.
  - PNG inflates with zlib-ng and unfilters while it inflates (8-bit: 650 ms to 350 ms;
    16-bit: 1.35 s to 0.66 s).
  - Embedded ICC profiles are found by reading only the file's header segments, not the
    whole file (up to 0.4 s saved per image). RAW files are read in one go.
- **Faster Color Mixer and Color Grading** in scene-linear projects (about 1.3x and 1.6x at
  24 MP): the band and zone adjustments come from tables by hue and lightness, and the hue turn is
  a rotation of Oklab a/b. Legacy projects are unchanged.
- **Quality: Fast** for Directional Blur and Bilateral Blur, on the CPU and the GPU. At 24 MP,
  Directional Blur takes 1.7 s instead of 8 s and Bilateral Blur 3 s instead of 13 s. Fast
  changes the look slightly, so High stays the default and existing projects are unchanged.
- **Find Node searches inside groups:** nodes in nested groups are listed as "Group > Node", and
  picking one opens its group, then selects and frames it.

## 0.19.0 (2026-10-02)

- **Find Node** (Ctrl+F, View > Find Node...): lists the nodes of the graph you are in, filtered by
  label or node name. Enter or a click selects the node and frames it.
- **Faster exports:** saving a 24 MP image takes 0.6-0.9 s instead of 3-5 s in every format.
  - PNG and TIFF compress with zlib-ng across all cores. Each block picks run-length matching for
    photos (as small as before, 3-4x faster) or full matching for graphics.
  - JPEG encodes in strips in parallel, joined with restart markers. The decoded pixels are the
    same as before.
  - The view transform (Standard and AgX) uses tables instead of per-pixel powers.
- **Faster RAW loading:** a 24 MP RAW opens about 1 s faster. Highlight Reconstruct runs across
  all cores, and the decoded image is converted to float in one parallel pass. Pixels are
  unchanged.
- **Faster Denoise** (about 25%) and faster image allocation for every node.
- **Faster Oklab conversions:** Basic's Vibrance and Saturation, Color Grading and Range Mask
  use a correctly rounded cube root that is about 4x faster than MinGW's (which was up to 2 ulps
  off). Basic's Vibrance pass on a 24 MP image went from about 1.1 s to 0.08 s.
- **Faster blurs and warps:** at 24 MP, Directional Blur is about 2.4x faster (34 s to 14 s), and
  Bilateral Blur and Sun Beams about 15% faster. Bilinear sampling (Transform, Crop, Lens
  Distortion and others) is inlined. Pixels are unchanged.
- `--render ... --timings` prints how long evaluation and saving took.
- **Fixed:** a NaN in a channel that drives a param (say a Math Divide of 0 by 0 into
  Wavelength) crashed NodeLab. It now counts as 0, as in Blender.
- **Fixed:** Noise Texture gave black (NaN) pixels on wide images at high Detail and
  Lacunarity, where the lattice coordinates overflowed.
- **Fixed:** a damaged or hand-edited project could hang or crash on load: param values are
  clamped to their ranges, values of the wrong type fall back to the default, and links to
  pins a node doesn't have are dropped.

## 0.18.0 (2026-10-01)

- **Preferences** (Edit > Preferences): interface, theme, viewer, compositor and new-project
  settings in one window, saved per user.
- **Themes:** NodeLab Dark, Blender, Darkroom, Midnight, High Contrast and Light presets. Every
  interface and Node Editor colour can be edited and saved as a custom theme.
- **Inspector overlay:** the selected node's settings float in the Node Editor's top-right
  corner, like Blender's sidebar. It's the default; Preferences switch it back to a panel.
- **Layout presets** (View > Layout): Default, Compositing, Photo, Side by Side and Node Focus.
  Reset Layout rebuilds the chosen one.
- **Reset to Defaults:** in the node right-click menu and the Edit menu. It keeps file paths and a
  RAW's defaults.
- **New projects' view transform:** Standard or AgX (with a look), chosen in Preferences.

## 0.17.0 (2026-10-01)

- **Denoise node:** noise reduction with Lightroom's Luminance, Detail, Color and Color Detail
  controls. It thresholds wavelet bands, so edges stay sharp. It runs on the GPU and in
  zoomed-in regions, and the preview matches the export.
- **Add Mask:** a Result toolbar button (Shift+M; M linear, Shift+R radial, K brush) adds a local
  adjustment in one step: a Basic before the Output with a new mask in its Factor, selected and
  ready to shape. The Inspector shows the mask with the Basic's sliders below it.
- **Range Mask node:** selects by luminance (Low, High, Smoothness) or by colour (picked colour
  and Amount), like Lightroom's Luminance and Color Range. A Mask input narrows it to a gradient
  or brush area.
- **Brush Mask Auto Mask:** strokes stop at edges where the photo's colour changes, as with
  Lightroom's Auto Mask. Brush Mask has a new Image input for it.
- **Library:** File > Open Folder (Ctrl+Shift+O, or drop a folder) shows a filmstrip of a
  folder's photos. Each photo's edit is saved automatically in a sidecar beside it
  (`IMG_1234.CR3.nlproj`) when you move to another photo. New photos start with Image Input →
  Denoise → Basic → Output (RAWs with colour noise reduction and the AgX view).
- **Culling:** Lightroom's keys rate (0-5), pick (P), reject (X) and unflag (U) the selected
  photos, and a filter shows picks, star ratings, rejects or edited photos.
- **Copy / Paste Edit** (Ctrl+Shift+C / Ctrl+Shift+V) applies one photo's node tree to the
  selected photos, each keeping its own file and rating.
- **Export Selected** exports the selected photos, each with its own edit.
- **Thumbnails** use a RAW's embedded preview; edited photos show a render of their edit, kept in
  the sidecar.
- **Faster brushing:** each dab repaints only the stroke being drawn, not every stroke so far
  (Brush Mask with 20 strokes: 57 ms to 7 ms per update).

## 0.16.0 (2026-10-01)

- **The viewers convert on the GPU:** with the GPU device on, the view transform, the clipping
  warnings and the histogram run on the card, and a result still on the card is converted there
  without being copied back first. On a 2.7 MP image this takes 15 ms instead of 80 ms (Standard)
  or 170 ms (AgX), so the viewer keeps up with slider drags.
- **Results stay on the GPU:** a viewer's result is no longer copied back to the CPU after each
  evaluation (12 ms for a 1 MP preview on an integrated GPU). The eyedropper copies it once,
  when it is used.
- **Zoomed-in detail on the GPU:** the sharp detail shown when zooming past the preview's
  resolution is now computed on the GPU too, with the same result as the whole image.
- **Exports on the GPU:** with the GPU device on, File > Export renders on the GPU at Full
  precision. The 38-node infrared graph on a 24 MP photo evaluates in 4.4 s instead of 18 s on
  an integrated GPU (saving the file takes the same time as before).
- `--device gpu` also works with `--screenshot` and `--script`, which otherwise use the CPU.

## 0.15.0 (2026-10-01)

- **Nearly every node runs on the GPU:** 79 of the 88 node types, up from 45.
  - Filters: Directional Blur, Bilateral Blur, Filter, Dilate / Erode, Kuwahara, Pixelate,
    Posterize, Glare and Sun Beams.
  - Transforms: Transform, Crop, Flip, Lens Distortion, Lens Correction, Displace, Map UV and
    Corner Pin.
  - Mattes: the keys, Color Spill, Box and Ellipse Mask, and the gradients.
  - Textures: Noise, Voronoi, Gradient, Wave, Checker and White Noise, with the CPU's exact hashes.
  - Reroute, Switch and Split. Reroute and Switch keep GPU values on the card.
  - Still on the CPU: Image Info, File Output, Brush Mask and Double Edge Mask.
- **Basic stays on the GPU with every slider:** Texture, Clarity, Dehaze and the scene-linear tone
  equalizer (Highlights and Shadows) run as GPU passes.
  - The guided filters, the dark channel, and Dehaze's airlight (an exact selection and a sum on
    the card) all run there.
  - Basic with all its local sliders on a 1 MP preview: 133 ms → 26 ms. At full resolution:
    320 ms → 66 ms.
- **Faster GPU blur:** shorter runs per thread keep more of the card busy, so Blur at size 30 on
  a 1 MP preview takes 7 ms instead of 10 ms.
- Preview times on Intel Iris Plus, CPU → GPU: effects 86 → 27 ms, infrared foliage 142 → 58 ms,
  the infrared preset 380 → 100 ms.
- **Known small differences:** Kuwahara can pick a neighbouring quadrant where two are almost
  equally smooth, so a few pixels on edges differ from the CPU's. Expression `floor()` at exact
  boundaries can also round the other way.

## 0.14.0 (2026-10-01)

- **Every per-pixel node runs on the GPU:** all Color nodes (including Basic, Color Mixer and
  Color Grading), Math, Map Range, Clamp, Threshold, Color Key, Float Curve, Wavelength,
  Blackbody, Set Alpha and Alpha Over join Mix, Blend, the expressions, Blur and Color Ramp.
  45 node types in all. Basic stays on the CPU while Texture, Clarity or Dehaze is used, and in
  scene-linear projects while Highlights or Shadows is.
- **Chains of per-pixel nodes run as one shader** (fusion, like Blender's GPU compositor). A
  node whose result goes only to the next per-pixel node is compiled into that node's shader, so
  the chain reads its inputs once and writes one image, at full float precision in between.
  Node Timings show a fused node's time on the node it ran in.
- **Normalize finds its percentiles on the GPU** (an exact radix select), so only two numbers come
  back instead of the whole image: 72 ms → 7 ms on the infrared preset's preview.
- **Faster GPU → CPU copies** for the viewer and CPU nodes (through a pixel buffer): 19 ms → 5 ms
  for a 1 MP preview on Intel graphics.
- On the infrared preset (Intel Iris Plus): preview about 120 ms → 100 ms, full resolution
  271 ms → 215 ms. GPU renders match the CPU's to within 1/255.

## 0.13.0 (2026-10-01)

- **GPU compositing, like Blender's compositor Device: GPU.** Nodes run as compute shaders on the
  graphics card (OpenGL 4.3, including Intel and AMD integrated graphics), and results stay on the
  card between GPU nodes.
  - On the GPU: Mix, Blend, Expression, Image Expression, Blur, Split/Combine RGB and HSV, Color
    Ramp and Normalize. Other nodes run on the CPU as before, with their inputs copied across.
  - **View > Compositor > Device** (GPU or CPU) and **Precision** (Auto stores images as half
    floats, which is faster; Full matches the CPU's floats). Saved as preferences, not in projects.
  - On the infrared preset: 846 ms → 271 ms at full resolution, 184 ms → about 120 ms for the
    preview, on Intel Iris Plus graphics.
  - Results match the CPU to within 1/255 in exported images. Exports and `--render` use the CPU
    unless you pass `--device gpu` (with `--precision half|full`); `--gpu-info` names the GPU.
  - A node that fails on the GPU (out of memory, a driver bug) runs on the CPU instead, and the
    status bar says so. Node Timings mark GPU nodes.
- **The window no longer freezes while a big graph updates.** Viewer images are prepared (view
  transform, clipping warnings, histogram) on a background thread instead of the UI thread, the
  image cache no longer holds its lock while decoding, and evaluation runs at a lower priority
  than the UI.
- Fixed: the window flashed black while moving or resizing panels and windows, and while typing
  in the Shift+A search.

## 0.12.1 (2026-09-30)

- **Embedded colour profiles on input:** JPEGs and PNGs with an ICC profile other than sRGB
  (Display P3 from iPhones, Adobe RGB, ProPhoto) are decoded through it to linear Rec.709. Before,
  they were read as sRGB, so their colours came out dull and shifted.
  - New **Embedded Profile** option on Image Input (on by default), and the Inspector names the
    profile it found.
  - Untagged and sRGB-tagged files decode exactly as before, and legacy projects ignore profiles.
  - Scene-linear projects with P3 or Adobe RGB images will look different (correct) when reopened.

## 0.12.0 (2026-09-30)

- **RAW default look, like darktable:** RAWs looked flat and about a stop darker than the camera's
  JPEG.
  - Choosing a RAW for an Image Input now sets **Baseline Exposure** to +0.7 EV and turns on
    **Compensate Camera Exposure**, which undoes the camera's exposure compensation (read from the
    EXIF). Both are visible, editable params.
  - A new project whose first image is a RAW switches its view transform to **AgX**, so the
    highlights a RAW keeps above 1 roll off instead of clipping.
  - Existing projects are unchanged: the new params default to 0 / off, and only choosing a file
    sets them.
  - The Original panel includes the Baseline Exposure.
- **Open an image from the command line:** `NodeLab.exe photo.CR2` (or Windows' Open with) starts a
  new project with it.
- Fixed: importing an image read the Image Input's params before they existed (worked by luck;
  crashed once the compiler inlined it).

## 0.11.0 (2026-09-30)

- **Full-resolution viewing:** zoom in past the preview and the Original and Result panels sharpen
  to the photo's real pixels, up to 100%.
  - Only the visible part is computed, from the full-resolution image, after the preview shows.
    Nodes run on just the area they need, with a margin for blurs and other neighbourhood filters.
  - Nodes that use whole-image statistics (Normalize, Basic's Dehaze) reuse the preview's, so
    the zoomed-in part matches the rest of the image.
  - A few nodes still need the whole image (groups, Pixelate, Directional Blur, Glare, Sun
    Beams). Zoomed in, a graph with one of these keeps showing the preview.
- **The preview follows the panel size:** from 768 to 2048 pixels on the long edge, instead of a
  fixed 1280. Large panels look sharper, and small ones evaluate faster.
- **Drafts while dragging:** when a preview takes longer than 100 ms, slider drags evaluate at half
  size and refine when you let go, like darktable and Lightroom.
- **Lower export memory:** exports drop each node's result once nothing else reads it, so a large
  photo no longer holds one full-size image per node.

## 0.10.0 (2026-09-30)

- **Export formats:** 16-bit PNG, 8/16-bit TIFF (Deflate) and **OpenEXR** (half or full float),
  in the Export window, File Output nodes and the command line.
  - PNG, JPEG and TIFF get the view transform and are tagged sRGB (PNG sRGB chunk; ICC profile in
    JPEG and TIFF).
  - OpenEXR stays scene-linear, as in Blender, and keeps values above 1.
- **EXIF in JPEG exports**, copied from JPEG sources or built from camera RAW metadata (camera,
  lens, exposure, ISO, date). The orientation is reset to upright.
- **Lanczos resizing in linear light** for smaller exports, replacing the box filter.
- PNG, TIFF and EXR compress with zlib in parallel: 8-bit PNGs are about a third smaller and
  16-bit PNGs export about 5x faster.
- The Export window's file name follows the chosen format.
- Command line: `--render out.tif|out.exr [--depth N]`; `--batch` takes `--tif`, `--exr` and
  `--depth N`.

## 0.9.0 (2026-09-30)

- **Camera RAW files** (CR2, CR3, NEF, ARW, DNG, RAF, ORF, RW2 and more) via LibRaw:
  - They load as scene-linear light with the as-shot white balance and the camera's colour
    matrix, upright. Basic's Temperature and Tint are relative to the as-shot white balance.
  - **Highlight Reconstruction** on Image Input: Clip, Blend, or Reconstruct (the default).
    Recovered highlights stay above 1 for Exposure and Highlights to bring back.
  - A fast half-size decode for the preview; the full decode happens at export and is
    OpenMP-parallel.
  - File dialogs, drag and drop and batch export accept RAW files.
- **EXIF orientation:** JPEGs load upright in scene-linear projects. Legacy projects keep pixels
  as stored.
- Image Input hides the params that don't apply: Color Space for RAW files, and Highlight
  Reconstruction for other images.
- Rendering and exporting no longer decode the source image twice.

## 0.8.0 (2026-09-30)

- **Develop maths for scene-linear projects** (Basic, Color Mixer, Color Grading), modelled on
  darktable:
  - **White balance** is a CAT16 adaptation along the blackbody locus. Temperature and Tint 0/0
    is no change.
  - **Highlights / Shadows / Whites / Blacks** form a tone equalizer: gains in stops from an
    edge-aware exposure mask, applied as RGB ratios, so there are no halos or hue shifts.
    Highlights can recover detail above white.
  - **Contrast** is a log-space S-curve around middle grey. **Clarity** and **Texture** work on
    log luminance.
  - **Vibrance, Saturation, the Color Mixer and Color Grading** work in Oklab/Oklch. Out-of-gamut
    colours are compressed toward grey instead of clipped.
  - Legacy sRGB projects render exactly as before.

## 0.7.0 (2026-09-30)

- **Scene-linear colour management**, as in Blender. New projects decode images to linear light
  and work on unbounded linear values:
  - **Color menu:** View Transform (Standard, **AgX**, Raw), Look (None, Punchy, Greyscale),
    view Exposure and Gamma. It applies only in the viewers and when exporting.
  - **Image Input > Color Space:** sRGB, Linear Rec.709 or Non-Color.
  - Exposure is an unclamped multiply. Colour nodes keep highlights above 1, and
    Brightness / Contrast pivots on middle grey.
  - Colour pickers show display values; the histogram and clipping show the view transform.
  - Projects saved before this version stay in the legacy sRGB working space and render
    byte-identically. **Color > Convert Project to Scene-Linear** switches one over.
  - Scene-linear projects use project format 2, which older NodeLab versions refuse to open
    instead of rendering them wrongly. Legacy projects still save as format 1.

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
