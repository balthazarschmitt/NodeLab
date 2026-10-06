# NodeLab Guide

NodeLab edits images with a graph of nodes. Every node takes data in on the left, does one job,
and passes the result out on the right. By wiring nodes together you build up a look that can be
changed at any point: nothing is baked in until you export.

This guide covers the basics, how the data on the wires works, every colour space, and every
node. The same text is available inside the app under **Help > Guide** (F1). With a node selected,
F1 (or the **Guide** button in the Inspector) jumps straight to that node's entry.

## Getting Started

### The Window

- **Original** (left): the image loaded by the first Image Input node.
- **Node Editor** (middle): the graph. Right-click empty space to add nodes.
- **Inspector**: every setting of the selected node, with larger editors for curves and ramps.
  By default it floats in the Node Editor's top-right corner while a node is selected; in
  **Edit > Preferences > Interface** (or **View > Inspector Overlay**) it can be a panel instead.
- **Result** (right): what the Output node receives, or the node you are previewing.
- **Status bar** (bottom): the preview's size and how long it took, then on the right an AI mask
  being computed (with its progress), NodeLab's memory, and the whole computer's RAM and CPU use.
  Hover it for details. The RAM turns orange when it is nearly full: Windows then swaps to disk,
  which slows everything down.

Every panel is a tab that can be dragged. Drop it on the edge of another panel to dock it there,
or outside the window to float it. **View > Layout** offers ready-made arrangements, like
Blender's workspaces:

- **Default**: the original and the result either side of the graph.
- **Compositing**: the result over a wide graph, the original as a tab behind it.
- **Photo**: the photo in the middle, its settings on the right and the graph underneath, like
  Lightroom's Develop module.
- **Side by Side**: before and after at equal sizes over the graph.
- **Node Focus**: a big graph with the images stacked on the right.

**View > Reset Layout** puts the chosen layout back.
**View > New Viewer** opens an extra image panel that can show any node (see Viewing Intermediate
Results below). **View > Node Timings** shows how long each node took above it (amber when it is
50 ms or more), so you can see what slows a graph down. Nodes that ran on the graphics card are
marked **GPU**.

### A First Graph

1. **File > Import Image** (Ctrl+I) or drag an image file onto the window. This creates an Image
   Input node.
2. Right-click in the graph, type `sat`, and press Enter to add a **Saturation** node.
3. Drag from the Image Input's **Image** output onto the Saturation's **Image** input.
4. Drag from the Saturation's output onto the **Output** node.
5. Drag the **Amount** slider on the node. The Result panel updates live.
6. **File > Export** (Ctrl+E) writes the result at full resolution (see Exporting below).
7. **File > Save** (Ctrl+S) writes a `.nlproj` project file. It stores the graph and the image
   paths (relative to the project), not the pixels, so keep images next to the project.

### Previewing

- **Ctrl+click** a node to show its output in the Result panel instead of the Output node.
  Ctrl+click it again (or use **View > Clear Node Preview**) to go back.
- **Ctrl+Shift+click** cycles through the outputs of a node that has several (for example the
  R, G and B outputs of Split RGB).
- **Preview size:** the preview works on a copy of the image sized to the panels (768 to 2048
  pixels on the long edge), so it stays fast.
  - Pixel sizes (blur radius, offsets) are measured in full-resolution pixels, so the preview
    matches the export.
  - Export always renders at full resolution.
- **Zooming in:** past the preview's pixels, the Original and Result panels sharpen to the
  photo's real pixels (up to 100%) once the view stops moving. Only the visible part is computed.
  - Statistics over the whole image (Normalize, Dehaze) come from the preview, so the detail
    matches the rest of the image.
  - A few nodes need the whole image (groups, Pixelate, Directional Blur, Glare, Sun Beams). In
    a graph with one of these, a zoomed-in view keeps showing the preview.
- **Slow graphs:** when a preview takes longer than 100 ms, dragging a slider shows a half-size
  draft that refines when you let go.
- **GPU compositing:** like Blender's compositor, **View > Compositor > Device** runs nodes on the
  graphics card (GPU, the default when the card supports OpenGL 4.3) or only on the processor
  (CPU).
  - Almost every node runs on the GPU: the Color, Converter, Filter, Transform, Matte and Texture
    nodes, including Basic with all its sliders. Image Info, File Output, Brush Mask and Double
    Edge Mask run on the CPU, and values are copied across for them. A graph made entirely of GPU
    nodes gains the most, because values stay on the card between them.
  - GPU results match the CPU's to within float rounding, with two exceptions on a few pixels.
    Kuwahara can pick a different but equally smooth quadrant on an edge. Expression `floor()`
    can round the other way at an exact boundary.
  - A chain of per-pixel nodes runs as one program on the card (fusion, as in Blender). A node
    whose result feeds only the next per-pixel node shows almost no time in Node Timings; its
    time is counted in the node it was folded into.
  - **Precision:** Auto keeps images in half floats, which is faster and plenty for a preview;
    Full matches the CPU exactly to within float rounding.
  - The viewers also use the GPU: the view transform, clipping warnings and histogram are
    computed on the card.
  - Zoomed-in detail and File > Export use the GPU too; exports always use Full precision. If a
    node fails on the GPU (for example when the card runs out of memory) it runs on the CPU
    instead, and the status bar (or the export log) says so.

### Viewing Intermediate Results

When a chain runs A → B → C, the Original panel shows A and the Result panel shows C. To watch B
at the same time, open an extra viewer:

- Right-click node B > **Open in New Viewer**, or select it and use **View > New Viewer**.
- The drop-down at the top of a viewer switches it to any node in the graph you are editing
  (listed left to right). **Follow Result** makes it show the same as the Result panel.
- **Pin Selected** points the viewer at the selected node.
- **Sync view** makes the viewer zoom and pan together with the Original and Result panels.

Viewers are ordinary panels: drag the tab to dock them next to the others. Open as many as you
like; they are saved with the project.

### Transparency

Images with alpha are drawn over a grey checkerboard, as in Blender's image editor, so transparent
and half-transparent areas (a crop's outside, a keyed background, Set Alpha) are easy to see. The
viewer's own background colour (Edit > Preferences > Viewer) shows around the image.

### Before / After

Like Lightroom's, with the mouse over the Result panel (or the **Before / After** toggle on its
toolbar):

- **Y** splits the Result: the original on the left of the divider, the edit on the right. Drag
  the divider's handle to move it. Both halves zoom and pan together.
- `\` (backslash) shows the original alone in the Result panel (press it again to go back).

The "before" is what the Original panel shows: the first Image Input.

### Histogram and Clipping

The Result panel's toolbar has these toggles (hover the Result panel to use the keys):

- **Histogram (H):** red, green, blue and luminance distribution of the result, drawn in the
  top-right corner. Dark tones are on the left, bright tones on the right.
- **Clipping (J):** paints pure-white or channel-clipped pixels red and pure-black pixels blue, like
  Lightroom's clipping warnings. The triangles in the histogram's corners light up when something
  clips; click one to toggle the warning.
- **Gamut:** soft proofing's gamut warning. Colours the export's **Color Space** (see Exporting)
  can't hold turn magenta, so with sRGB you see which colours an sRGB file would clip, and with
  Display P3 or Adobe RGB how much more a wider space keeps. It needs a scene-linear project with
  the Standard view (other views already map everything into sRGB).

### On-image Controls

Some nodes are edited directly on the Result panel while they are selected:

- **Crop:** the Result shows the whole frame with the crop rectangle on top. Drag a corner or edge
  to resize, drag inside to move, and drag outside the frame to straighten (rotate). With an
  Aspect preset the frame keeps its ratio. Deselect the node to see the cropped result.
  - **Crop guide overlays:** with the mouse over the Result, **O** cycles Lightroom's overlays
    inside the frame: Grid, Thirds, Diagonal, Triangle, Golden Ratio, Golden Spiral and Aspect
    Ratios. **Shift+O** turns the Triangle and Golden Spiral to face another corner. View > Crop
    Guide Overlay picks one directly. A fine grid replaces it while you straighten.
- **Perspective:** a fine grid covers the image. With Upright set to Guided, drag along a line
  that should be straight to draw a guide (up to four); drag a guide's ends to move them and
  Alt+click a guide to remove it. Steep guides are blue (they become vertical), flat ones pink
  (horizontal).
- **Radial Gradient, Box Mask, Ellipse Mask:** drag the centre to move, the side and bottom
  handles to resize, and the handle above to rotate (Shift snaps to 15 degrees).
- **Linear Gradient:** drag the Start or End handle, or the middle one to move both.
- **Brush Mask:** paint with the left mouse, Alt+paint to erase, and `[` / `]` to change the
  brush size. Turn on Auto Mask to keep strokes from spilling over edges onto other colours.
- **Spot Removal:** click a blemish to add a spot (its source is picked automatically), press
  `/` for another source, or drag the source circle yourself (see Spot Removal under Filter).
- **Mask Overlay (O):** a selected mask node is tinted red over the image, so you can see what it
  covers even before it is wired into anything.

Middle-drag still pans and the wheel still zooms while these controls are shown.

**Loupe Overlay** (View > Loupe Overlay), as in Lightroom: **Grid** draws a grid over the Result
(Grid Size sets its spacing in screen pixels) and **Guides** a horizontal and a vertical line you
can drag (drag where they cross to move both). Use them to check that horizons and verticals are
straight. Center Guides puts them back in the middle.

**Add Mask** (on the Result toolbar) makes a local adjustment in one step, like Lightroom's
"Create New Mask". It inserts a **Basic** labelled "Mask N" right before the Output and wires a
new mask into its Factor: Linear Gradient, Radial Gradient, Brush (its Image wired for Auto
Mask), Luminance Range, or the AI masks Subject, Sky and Background (Select Subject inverted). The mask is selected, so you can shape or paint it straight away, and
the Inspector shows its settings with the Basic's sliders underneath. With the mouse over the
Result: Shift+M opens the menu, M adds a Linear Gradient, Shift+R a Radial Gradient and K a
Brush. One Ctrl+Z removes the whole adjustment.

### Picking Colours from the Image

Every colour setting (for example the Key Color of the keying nodes, or the Checker Texture's
colours) has an eyedropper.

1. Press **Pick** next to the colour in the Inspector, or click the colour on the node and choose
   **Pick from Image**.
2. Move over the Original, the Result or a viewer. A swatch next to the cursor shows the colour
   under it.
3. **Click** to take a single pixel, or **drag a rectangle** to take the average colour of an
   area, which is better for noisy or textured surfaces such as a green screen.

Right-click or Esc cancels. Middle-drag still pans while picking. The pick is one undo step.

## Wires and Data Types

Every pin has one of three types, shown by its colour.

| Type | Colour | What it carries |
|---|---|---|
| Image | amber | A full colour picture: red, green, blue and alpha for every pixel. |
| Channel | gray | One number for every pixel, like a grayscale image. Masks, keys and single colour components are channels. |
| Number | blue | A single number for the whole image. |

### Automatic Conversions

You can connect pins of different types; NodeLab converts on the way.

- **Channel to Image:** the channel becomes a gray image (R = G = B = the channel, alpha 1).
- **Image to Channel:** the image's luminance (Rec. 709 luma, see Colour Spaces).
- **Number to Channel:** the same value on every pixel.

### Sliders Become Inputs

Many inputs have a slider next to them when nothing is connected, for example Saturation's
**Amount**. Connect a channel to that pin and the value comes from the channel instead, one value
per pixel. This is the core trick of NodeLab: **Split RGB > R** into **Saturation > Amount**
makes red areas more saturated and non-red areas grayer.

A value of 0..1 on a channel usually means "none to full". Sliders keep values in their useful
range. Colour nodes clamp their output to 0..1; Math and Converter nodes don't, so you can do
arithmetic with values outside that range (use a Clamp node or the Clamp option when needed).

### Fan-out

One output can feed as many inputs as you like. Each input has exactly one wire; connecting a new
wire to an occupied input replaces the old one.

## Colour Management

New projects are **scene-linear**, like Blender. Pixel values are proportional to the light in the
scene, and nothing inside the graph limits them to 0..1:

- **Loading:** images are decoded from sRGB to linear light, as set by each Image Input's
  **Color Space** (sRGB for photos, Linear Rec.709 for linear files, Non-Color for masks and data).
- **Editing:** Exposure is a plain multiply, so +1 stop doubles every value, highlights included.
  Blurs, glows and blends mix light realistically. Colour nodes clamp only negative values, so
  highlights above 1 survive until the end.
- **Viewing and exporting:** a **view transform** turns scene values into display values, only in
  the viewers and when exporting (Export, File Output, `--render`), never inside the graph. The
  Original panel is shown through it too, so it can be compared with the Result.

The **Color** menu holds the project's settings (Blender's Render Properties > Color Management):

- **View Transform:**
  - **Standard:** the plain sRGB curve. A photo with no edits exports exactly as it was loaded.
    Values above 1 clip.
  - **AgX:** Blender's filmic transform. Highlights roll off smoothly up to about 6.5 stops over
    middle grey, and very bright saturated colours fade toward white instead of clipping to a flat
    primary.
  - **Raw:** the linear values, unencoded, for checking data.
- **Look** (AgX only): None, **Punchy** (more contrast and saturation) or **Greyscale**.
- **Exposure** (stops) and **Gamma** adjust the view only. They're handy for checking highlight
  detail without changing the graph, but they also apply when exporting. Right-click resets them.

Colour pickers show and edit display (sRGB) values, as Blender's do; the node receives the
matching linear colour. The eyedropper picks the scene values under the cursor. The histogram and
clipping warnings show the view-transformed image.

**Legacy projects.** Projects saved before 0.7 work on sRGB-encoded values, as NodeLab always did,
and open unchanged: they render exactly as before and the Color menu's view settings are disabled.
**Color > Convert Project to Scene-Linear** switches one over. Nothing is added to the graph, so
it will look different: curves, levels and blends tuned on sRGB values may need adjusting. Ctrl+Z
undoes the conversion. Scene-linear projects can't be opened by NodeLab 0.6 or older.

## Colour Spaces

An RGB pixel can be described in several other ways. Each **Split** node takes an image apart into
the components of one colour space, and the matching **Combine** node builds an image back from
them. Between the two you can edit, swap or rewire components. For example, blur only the colour
while keeping detail sharp, or drive one component from another.

All components are scaled to fit comfortably on channels. The ranges below are the ones NodeLab
uses, which are not always the textbook units.

### RGB and sRGB

- **What it is:** red, green and blue light, each 0..1. This is how images are stored and shown.
- **sRGB:** the values in your image file, with the display's gamma curve built in. 0.5 looks like
  a middle gray, even though it is only about 21% of the light of white. Legacy projects work on
  these values.
- **Linear light:** physically proportional to the amount of light. Adding light, blurring and
  glows are more realistic in linear. Scene-linear projects (the default, see Colour Management)
  work in linear light throughout. In a legacy project, use **Convert Colorspace** to go
  sRGB > Linear before such operations and Linear > sRGB after.
- **Alpha (A):** opacity, 1 = solid, 0 = transparent. Every Split node has an A output and every
  Combine node an A input.

### HSV (Hue, Saturation, Value)

- **H (hue):** the colour's position on the colour wheel, 0..1 instead of 0..360°. Red 0, yellow
  0.167, green 0.333, cyan 0.5, blue 0.667, magenta 0.833, and back to red at 1. It wraps: 1.1 is
  the same as 0.1.
- **S (saturation):** 0 = gray, 1 = the purest version of the hue.
- **V (value):** the brightest of R, G and B. A pure red and white both have V = 1.
- **Good for:** hue shifts, picking colours by hue, making things more or less colourful.
- **Watch out:** V doesn't match perceived brightness (pure blue looks much darker than pure yellow
  at the same V), and hue is meaningless for grays.

### HSL (Hue, Saturation, Lightness)

- **H:** the same hue as HSV.
- **L (lightness):** the average of the brightest and darkest of R, G, B. 0 is black, 1 is white,
  and pure colours sit at 0.5.
- **S:** saturation relative to the lightness.
- **Good for:** when you want white at the top of the lightness scale, unlike HSV where white and
  pure colours share V = 1.

### Lab (CIE L\*a\*b\*)

- **What it is:** a perceptual colour space, designed so equal numeric steps look like equal
  visual steps. NodeLab uses the D65 white point.
- **L:** perceived lightness, 0 black to 1 white (L\* / 100).
- **a:** green (negative) to red/magenta (positive), about -1..1 (a\* / 128).
- **b:** blue (negative) to yellow (positive), about -1..1 (b\* / 128).
- **Good for:** adjusting brightness without touching colour (edit L only), or pushing colour
  casts: raise b for warmer, lower it for cooler. Blurring a and b while keeping L sharp gives a
  soft, painterly colour bleed.
- **Watch out:** combining extreme L, a and b values can produce colours that RGB can't show; they
  are clipped.

### YCbCr (Rec. 709, full range)

- **What it is:** the video and JPEG way of splitting brightness from colour.
- **Y (luma):** weighted brightness, 0..1: 0.2126 R + 0.7152 G + 0.0722 B.
- **Cb:** blue minus luma, centred on 0.5 (0.5 = no blue/yellow shift).
- **Cr:** red minus luma, centred on 0.5 (0.5 = no red/cyan shift).
- **Good for:** chroma keys, video-style glitches (offset or blur Cb/Cr and not Y, like bad
  video compression), and chroma subsampling looks (pixelate Cb and Cr only).

### YUV (BT.601)

- **What it is:** the analog TV version of the same idea, with older weights
  (0.299 R + 0.587 G + 0.114 B).
- **Y:** luma, 0..1.
- **U:** blue difference, about -0.44..0.44, centred on 0.
- **V:** red difference, about -0.62..0.62, centred on 0.
- **Good for:** VHS and analog TV looks. Blur U and V horizontally, shift them sideways, or add
  noise to them.

### Luminance Methods

The **Luminance** node turns an image into one brightness channel. Its methods:

- **Rec.709 luma:** 0.2126 R + 0.7152 G + 0.0722 B. Matches how bright colours look; the default,
  and what automatic Image-to-Channel conversion uses.
- **Average:** (R + G + B) / 3. Simple, but blues look too bright and greens too dark.
- **Max (HSV value):** the brightest component.
- **Lab lightness:** L of Lab, the most perceptually even.

### Light and Colour Temperature

- **Wavelength:** light of a single wavelength, 360..830 nm, as the colour the eye sees. Visible
  light runs from about 380 nm (violet) through 450 (blue), 520 (green), 580 (yellow) and 620
  (orange) to 700 nm (deep red). Beyond about 780 nm is infrared and below 380 ultraviolet;
  both fade to black here, because the eye can't see them.
- **Blackbody temperature:** the colour of an object glowing from heat, in Kelvin. Candle light is
  about 1900 K, tungsten bulbs 2700-3200 K, daylight 5500-6500 K, and overcast sky 7000-10000 K.
  Lower is orange, higher is bluish.

## Node Reference

Pins are listed as inputs, then outputs. When an input pin also has a slider, the slider sets its
value while nothing is connected.

### Input / Output

**Image Input**
- Loads an image file: PNG, JPEG, BMP or TGA (8 or 16 bits per channel), **TIFF** (8 or 16-bit,
  or 16/32-bit float; uncompressed, LZW, Deflate or PackBits, as scanners and other editors write
  them), **OpenEXR** (the RGBA, or the first layer with colour, as Blender's multilayer files
  have), **WebP**, **JPEG XL** or **AVIF** (with their colour profiles and orientation), or a
  camera **RAW** file (CR2, CR3, NEF, ARW, DNG, RAF, ORF, RW2, PEF and most others, decoded by
  LibRaw). NodeLab's own TIFF, EXR, WebP, JPEG XL and AVIF exports open again this way.
- Output: Image.
- **Color Space** (as in Blender) says how the file's values are decoded in a scene-linear
  project: **sRGB** (photos and most images) converts to linear light; **Linear Rec.709** and
  **Non-Color** (masks, depth, data) load the values as they are. Legacy projects always load them
  as they are. It is hidden for RAW files, which are always linear camera data. As in Blender,
  OpenEXR and float TIFF files start as Linear Rec.709 and other images as sRGB.
- **Embedded Profile** (on by default, sRGB Color Space only): photos that carry an ICC colour
  profile other than sRGB are decoded through it, as Lightroom does. iPhone photos (Display P3)
  and camera or editor exports in Adobe RGB or ProPhoto otherwise look dull and shifted.
  - The Inspector shows the profile it found, e.g. "Profile: Display P3". Nothing is shown for
    untagged and sRGB files, which decode exactly as before.
  - Colours outside Rec.709 (P3's saturated reds and greens) come out with negative channels,
    which the Develop nodes and the view transform handle like a RAW's.
  - Profiles built from lookup tables, and CMYK or Lab ones, can't be applied; the Inspector says
    so and the file is read as sRGB.
  - Legacy projects read files as stored and ignore profiles.
- In scene-linear projects, JPEGs are turned upright from their EXIF orientation, as cameras and
  phones expect. Legacy projects keep the pixels as stored, so old edits still line up.
- **RAW files** load as scene-linear light with the camera's "as shot" white balance, upright,
  with no tone curve.
  - Basic's Temperature and Tint are relative to the as-shot white balance, as in Lightroom.
  - **Default look:** cameras expose to protect highlights, so the sensor data looks about a stop
    darker than the camera's JPEG. Choosing a RAW (File > Import, Browse, dropping it on the
    window, or a RAW batch) sets two params, as darktable does:
    - **Baseline Exposure** +0.7 EV.
    - **Compensate Camera Exposure** on. This undoes the exposure compensation set on the camera,
      so a shot taken at -1 EV to save the sky comes in at normal brightness.
  - Both are ordinary params you can change; set them to 0 / off for the data exactly as shot.
    Switching from one RAW to another keeps them. Projects saved before 0.12 have them at 0 / off,
    so they render as before.
  - The Original panel shows the RAW with these applied, as Image Input outputs it.
  - **View transform:** when a RAW is the first image of a new project, the view switches to
    **AgX**, which rolls off the highlights a RAW keeps above 1 instead of clipping them (in the Color
    menu; its Punchy look adds contrast back). A project whose view settings you have
    changed keeps them.
  - The preview uses a fast half-size decode. The full-size decode (one to three seconds for a
    20 MP file) happens only when exporting.
  - In legacy projects a RAW is encoded to sRGB and clipped like a JPEG.
- **Highlight Reconstruction** (RAW only) handles areas where some of the sensor's colour channels
  clipped, such as a bright sky or a sunset:
  - **Clip** cuts every channel at white. Pulled-down highlights turn flat grey, or shift colour
    where one channel clipped first.
  - **Blend** mixes the clipped channels with the unclipped ones. This keeps detail but tends
    toward pink or grey.
  - **Reconstruct** (the default) rebuilds the missing colour from neighbouring pixels, so a
    sunset stays orange as you lower Exposure or Highlights.
  - Blend and Reconstruct keep the recovered highlights above 1, so Exposure or Highlights can
    bring them back.
- The first Image Input in the graph sets the project's working size and is shown in the Original
  panel. You can have as many as you like, for example to blend two photos or to load a mask.

**Output**
- The final result, shown in the Result panel and written by File > Export.
- Input: Image.

**Number**
- A single value you can wire into any Number or Channel input, handy for driving several nodes
  from one slider.
- Output: Value (Number).

### Color

These adjust colour and tone. In legacy projects their outputs are clamped to 0..1. In
scene-linear projects only negative values are clamped, so highlights above 1 pass through, as in
Blender. Invert, Posterize and the curve-based nodes still work on 0..1.

**Brightness / Contrast**
- Brightness adds or removes light (-1..1).
- Contrast pushes values away from (positive) or towards (negative) middle gray.
- In scene-linear projects contrast pivots on middle grey (0.18) and bends values in stops, so
  dark values never go below black and highlights stay unclamped.

**Saturation**
- Amount 0 = grayscale, 1 = unchanged, above 1 = more colourful (up to 4).
- Connect a channel to Amount to vary saturation per pixel.

**Hue Shift**
- Rotates every colour around the colour wheel by Degrees (-180..180). 120° turns red into green,
  green into blue, and blue into red.

**Exposure**
- Brightens or darkens in photographic stops: +1 doubles the light, -1 halves it.
- Works in linear light, so highlights behave like a camera, not a simple brightness slider.
- In scene-linear projects it is a plain, unclamped multiply: values above 1 are kept for the view
  transform (or a later node) to handle.

**Gamma**
- Bends the midtones: below 1 brightens them, above 1 darkens them. Black and white stay put.

**Levels**
- In Black / In White: input values at or below In Black become black, at or above In White
  become white. Moving them inward increases contrast.
- Gamma: midtone brightness between the two.
- Out Black / Out White: the output range. Raising Out Black gives faded blacks; lowering Out White
  gives muted highlights.
- Channel: apply to all of RGB or to a single channel.

**Curves**
- The classic tone curve. Tabs for Master (all channels), R, G and B.
- Click the graph to add a point, drag to move, right-click a point to delete it.
- An S-shaped curve adds contrast. Lifting the bottom-left point fades the blacks. Different
  curves per channel give colour grades, for example blue lifted in shadows and lowered in
  highlights.
- Factor mixes between the original (0) and the full effect (1).

**Invert**
- Photographic negative: 1 - value per channel.
- Factor blends between original and inverted, useful for partial solarisation looks.

**Film Negative**
- darktable's negadoctor: makes a scanned or camera-copied film negative positive the way a
  darkroom print does, which a plain Invert can't (the film's orange mask and its log response
  leave an Invert muddy and blue).
- **Film Base Color:** the colour of clear film (the unexposed border between frames, or the
  rebate). Pick it from the image with the eyedropper; it prints black and removes the mask's cast.
- **D Max:** the density of the brightest part of the scene above the base; raise it until the
  highlights stop clipping. **Scan Offset** shifts every density (like exposing the scan more or
  less). **Density Correction** scales each channel's density, to neutralise a cast in the
  highlights.
- **Contrast** is the paper grade: how many stops the film's range spans. **Print Exposure**
  brightens or darkens in stops, and **Black** sets the paper's black (raise it for deeper blacks,
  lower it below zero to lift them).
- **Type:** Black & White uses one density for all channels, for black and white film.
- Put it straight after the Image Input of the scan, before any other colour work. In legacy
  projects the scan is decoded to linear light first.

**Split RGB / Combine RGB**
- Split takes an image apart into R, G, B and A channels. Combine builds an image from four
  channels (unconnected ones use their slider). Like Blender, Combine does not clamp, so it can
  also pack three data channels (masks, values above 1 or below 0) into one image for an
  Expression to read as r, g and b.
- These are the most useful nodes in NodeLab: swap channels for false colour, drive other nodes
  from a colour, or process one channel on its own.

**Split HSV / Combine HSV**
- Hue, Saturation and Value, each 0..1 (see Colour Spaces).

**Split HSL / Combine HSL**
- Hue, Saturation and Lightness, each 0..1.

**Split Lab / Combine Lab**
- L 0..1, a and b about -1..1.

**Split YCbCr / Combine YCbCr**
- Y 0..1, Cb and Cr centred on 0.5.

**Split YUV / Combine YUV**
- Y 0..1, U and V centred on 0.

**Luminance**
- One brightness channel from an image. Method: Rec.709 luma, Average, Max (HSV value) or Lab
  lightness.

**Hue Correct**
- Three curves across the hue spectrum. The strip under the graph shows which colour each position
  is.
- **Hue:** shifts the hue of colours in that part of the spectrum. The middle line means no
  change. For example, push the green part up or down to turn foliage red or blue.
- **Saturation / Value:** raise or lower saturation or brightness only for those hues. The middle
  line is no change; the top doubles and the bottom removes.
- Factor blends with the original.

**Color Balance**
- **Lift / Gamma / Gain:** colour wheels for shadows, midtones and highlights. Each is an RGB
  multiplier where 1 is neutral. For example, Lift slightly blue plus Gain slightly orange gives a
  teal-and-orange grade.
- **Offset / Power / Slope (ASC CDL):** the film-industry version. Slope multiplies, Offset adds,
  and Power is a gamma per channel.
- Factor blends with the original.

**Basic**
- Lightroom's Basic panel in one node, with its sliders in the Inspector (the node itself stays
  small). All sliders except Exposure run from -100 to 100, with 0 as no change.
- **White Balance:** Temperature (blue to yellow) and Tint (green to magenta), applied in linear
  light.
- **Tone:** Exposure (in stops), Contrast, then Highlights and Shadows (recover or open up the
  bright and dark tones), and Whites and Blacks (set the end points).
- **Auto** (in the Inspector, by Tone) is Lightroom's Auto Tone: it looks at the image coming into
  the node and sets Exposure, Contrast, Highlights, Shadows, Whites and Blacks for a balanced
  starting point, which you can then adjust. One Ctrl+Z undoes it.
- **Presence:** Texture (fine detail), Clarity (local midtone contrast), Dehaze (removes or adds
  atmospheric haze), Vibrance (boosts muted colours more than saturated ones and protects skin
  tones), and Saturation.
- Factor blends with the original. Wire a mask into it to make a local adjustment. In
  scene-linear projects it blends in stops, as Lightroom does, so a 50% mask gives half the
  adjustment (Exposure -2 becomes -1 EV). Color Grading and Color Mixer work the same way.
- **In scene-linear projects** Basic uses darktable-style maths on the linear light:
  - White balance is a CAT16 chromatic adaptation. Temperature moves the assumed light along the
    blackbody (Planckian) locus, and Tint moves it across toward green or magenta. Brightness stays
    put.
  - Exposure multiplies without clipping, so detail above white can be brought back.
  - Highlights, Shadows, Whites and Blacks work like a **tone equalizer**. Each pixel is brightened
    or darkened by a number of stops that depends on its exposure. Highlights and Shadows read an
    edge-aware smoothed exposure mask, so whole regions move together without halos at their edges.
    Colour ratios are kept, so hues don't shift.
  - Contrast is an S-curve in stops around middle grey. Clarity and Texture work on log luminance.
  - Vibrance and Saturation scale chroma in Oklab, which keeps lightness and hue. Colours pushed out
    of gamut are pulled toward grey rather than clipped.
  - Legacy (sRGB) projects keep the earlier maths.

**Color Mixer**
- Lightroom's HSL panel: Hue, Saturation and Luminance for eight colour bands (Red, Orange,
  Yellow, Green, Aqua, Blue, Purple and Magenta).
- The Inspector has one tab per property and an All tab. Bands blend smoothly into each other, and
  grey pixels are left alone.
- **Uses:** darken a blue sky (Blue Luminance down), make foliage autumnal (Green Hue towards
  yellow), or calm down a loud colour.
- In scene-linear projects the bands are measured in Oklch, a perceptual space, so changing
  Saturation or Hue keeps a colour's lightness, and Luminance changes it in stops.

**Color Grading**
- Lightroom's colour wheels: Shadows, Midtones and Highlights, plus Global for the whole image.
  Drag in a wheel to pick the tint's hue (the angle) and strength (the distance from the centre).
  Shift drags finely, and double-click resets the wheel.
- The slider under each wheel brightens or darkens that range.
- **Blending:** how much the three ranges overlap. **Balance:** moves the split between shadows
  and highlights (positive favours the highlights tint).
- **Uses:** teal shadows with warm highlights, or a gentle overall warm or cool cast.
- In scene-linear projects the ranges are chosen by Oklab lightness and the tint is added in
  Oklab, so blacks stay neutral and brightness is kept.

**Tone Equalizer**
- darktable's tone equalizer: brightens or darkens parts of the image by how exposed they are,
  without flattening them. Nine sliders set a gain in stops (-2..2) for each exposure from -8 EV
  (deep shadows) to 0 EV (white); the curve through them is smooth, and a slider's value is the
  exact gain at its exposure.
- Each pixel reads its exposure from a mask: the image's brightness smoothed within regions but
  not across edges (a guided filter), so a whole face or sky moves together and keeps its local
  contrast. **Show Mask** displays it (dark is -8 EV, white 0 EV).
- **Smoothing:** the mask's blur radius in % of the image's long edge (0 uses each pixel's own
  brightness, like a curve). **Feathering:** how closely the mask follows edges; raise it if halos
  appear around high-contrast edges.
- **Mask Exposure** shifts the mask, and **Mask Contrast** spreads it around -4 EV, so its tones
  cover the sliders you want to use.
- **Uses:** lift the shadows of a backlit portrait (-6 and -5 EV up) while holding the sky (-1
  and 0 EV down), a gentle HDR look.

**Color Equalizer**
- darktable's color equalizer: **Hue** (degrees), **Saturation** and **Brightness** for eight hues
  (Red, Orange, Yellow, Green, Cyan, Blue, Lavender and Magenta) on Oklch's perceptual hue circle.
- Each colour takes a blend of the hues near it; **Smoothness** widens the blend (higher is
  softer transitions, lower targets a hue more precisely). Greys and nearly grey colours are left
  alone, and Brightness moves perceptual lightness rather than exposure, so a colour keeps its
  saturation.
- Compared with Color Mixer: the hues are placed where colours are perceptually distinct
  (including skin's orange and a lavender between blue and magenta), and the blend is smoother.

**Tone Map**
- Compresses very bright values (for example after Exposure, Glare or Add blends) back into 0..1
  smoothly instead of clipping.
- Reinhard is gentle, with White Point as the value that maps to white. Filmic (ACES fit) gives a
  contrasty, film-like roll-off.
- Exposure adjusts before mapping.

**Convert Colorspace**
- Converts between sRGB, linear and gamma 2.2 encodings.
- Use sRGB > Linear before physically based operations (Exposure-like maths, adding light,
  blurring highlights) and Linear > sRGB afterwards.

### Mix

**Mix**
- Crossfades from A to B by Factor (0 = A, 1 = B).
- Connect a mask channel to Factor to show B only where the mask is white.

**Blend**
- Combines A (base) and B (top layer) with a Photoshop-style blend mode, then crossfades by Factor.
- Clamp limits the result to 0..1.
- Modes:
  - **Mix:** B over A.
  - **Darken / Lighten:** the darker or lighter of the two, per channel.
  - **Multiply:** darkens; white in B has no effect. Good for shadows and colour tints.
  - **Screen:** lightens; black in B has no effect. Good for glows and light leaks.
  - **Color Burn / Color Dodge:** extreme darken and lighten with strong contrast.
  - **Add:** adds light, like a double exposure. **Subtract** takes it away.
  - **Overlay:** Multiply in darks and Screen in lights; adds contrast and texture.
  - **Soft Light:** a gentler Overlay. **Linear Light:** a stronger one.
  - **Difference:** absolute difference; identical areas turn black. Good for psychedelic looks and
    for comparing images.
  - **Exclusion:** a softer Difference.
  - **Divide:** A / B; dividing by a blurred copy of the image flattens lighting.
  - **Hue / Saturation / Color / Value:** take that HSV property from B and the rest from A. For
    example, Color mode puts B's colours onto A's brightness.

**Alpha Over**
- Places Foreground on Background using the foreground's alpha, like stacking layers.
- Premultiplied: turn on if the foreground's colours are already multiplied by alpha (for
  example the Image output of a keyer).
- Factor fades the foreground.

**Layer Stack**
- Photoshop's layers in one node: any number of images stacked on each other. **Layer 0** is the
  bottom and each higher layer is drawn over everything below it. On the node, the pins run from
  Layer 0 down to the top layer.
- Connecting the top layer adds an empty one above it, so there's always a pin for the next image.
- Each layer has an **Opacity** (0 shows the layers below, 1 covers them where the layer is
  opaque); its slider is on the layer's pin, and wiring a mask into the pin gives the layer a
  layer mask. The layer's own alpha counts too, so transparent parts of a layer always show
  through.
- Each layer has a blend **Mode** (in the Inspector): Normal, or Blend's modes (Multiply, Screen,
  Overlay, Soft Light, Color, Value...), blended with everything below it.
  The bottom layer's mode doesn't matter.
- The Inspector lists the layers top first, as Photoshop's Layers panel does: move a layer up or
  down (its wires go with it), remove it, or **Add Layer**.
- The bottom connected layer sets the size; other sizes are stretched to it, as in Mix.

**HDR Merge**
- Lightroom's Photo Merge > HDR: bracketed exposures of the same scene merged into one image with
  the range of all of them. Connect the brackets in any order; as on Layer Stack, connecting the
  last pin adds another.
- The result is at the middle exposure's brightness (by mean brightness), with the highlights
  from the darker frames and the shadows from the brighter ones. Each frame counts most where it
  is well exposed and not at all where it is clipped.
- It is scene-linear and goes above 1: follow it with Basic (Exposure, Highlights), Tone Map or
  the view transform to bring the range into the display's. Use it in a scene-linear project;
  legacy projects get sRGB-encoded values back.
- **Auto Align** finds each frame's shift from the middle one (median threshold bitmaps, which
  look the same at any exposure), for handheld brackets. It shifts but doesn't rotate.
- **Deghost** leaves a frame out where it disagrees with the middle one (something moved between
  shots: people, leaves, water). Low, Medium and High catch smaller and smaller differences.
- The first connected image sets the size.
- **Uses:** high-contrast scenes (interiors with windows, sunsets) shot as brackets.

**Panorama Merge**
- Lightroom's Photo Merge > Panorama: overlapping photos stitched into one. Connect them in any
  order: their overlaps are found from the pictures (corner features matched between every
  pair), and the photo with the most matches is the centre.
- **Projection:** **Spherical** (wide and tall panoramas; straight lines bend), **Cylindrical**
  (wide panoramas; verticals stay straight), **Perspective** (straight lines stay straight, but
  the edges stretch, so it suits only a few photos; flat subjects such as a wall or a document).
- Brightness differences between the photos are evened out, and the seams are feathered.
- Areas no photo covers are transparent. **Auto Crop** cuts the result to the largest rectangle
  with no empty areas (Lightroom's Auto Crop); otherwise crop it yourself, or fill the gaps with
  Remove.
- Photos that match nothing are left out; if nothing matches, the first photo comes out as it is.
- Shoot with about a third of each photo overlapping the next, the same exposure and focal
  length, turning around the lens rather than walking. The matching runs on pictures scaled to
  800 pixels, so the proxy and the export stitch the same way.

### Converter

Converters work on channels and numbers. They don't clamp unless they have a Clamp option.

**Color Ramp**
- Maps a 0..1 channel onto a gradient of colours. Low values take the colour at the left of the
  ramp, high values the colour at the right.
- Click the ramp to add a stop, drag stops to move them, and right-click a stop to delete it.
- Interpolation: Linear, Constant (hard bands), Ease or Smooth.
- Outputs the colour Image and the ramp's Alpha.
- **Uses:** false colour (Luminance into a ramp gives thermal-camera or gradient-map looks),
  posterised colour bands, custom colour grades, and remapping masks.

**Color Key**
- Selects a colour range, like a colour-range selection in a photo editor.
- Hue (0..360°) and Hue Range pick the hues; Sat Min/Max and Value Min/Max limit the saturation
  and brightness; Softness blurs the edge of the range.
- The Inspector shows Hue as a colour wheel: click or drag on it to pick the hue (double-click
  resets it). The keyed region is outlined on the wheel: Hue ± Hue Range around it, from Sat Min
  to Sat Max out from the grey centre.
- Outputs a Mask (1 = selected) and the Image with everything else black. Invert flips it.
- **Uses:** "only the greens" or "only the sky" as a mask for Mix or for any node's Factor.

**Map Range**
- Remaps Value from the range From Min..From Max to To Min..To Max. For example, 0.2..0.6 to
  0..1 stretches that band to full range.
- Interpolation: Linear, Smooth Step (eased), or Stepped (4 or 8 levels).
- Clamp keeps the result inside the target range.

**Math**
- Per-pixel arithmetic on two channels, A and B.
- **Basic:** Add, Subtract, Multiply, Divide, Power, Logarithm (base B), Square Root, Absolute,
  Minimum, Maximum.
- **Compare:** Less Than, Greater Than (1 or 0).
- **Rounding:** Modulo, Floor, Ceil, Round, Fraction (the part after the decimal point).
- **Waves:** Sine, Cosine.
- **Snap:** round to a multiple of B.
- **Ping-Pong:** bounces back and forth between 0 and B.
- Clamp limits the result to 0..1.

**Clamp**
- Limits Value to Min..Max.

**Threshold**
- Outputs 1 where Value is above Threshold and 0 below it. Softness makes a smooth transition
  instead of a hard edge.
- **Uses:** turn any channel into a mask.

**Expression**
- A formula evaluated for every pixel, producing a channel.
- **Variables:**
  - `r`, `g`, `b`, `a`: the input image's pixel, 0..1.
  - `in1`, `in2`: the two extra channel inputs (or their sliders).
  - `x`, `y`: pixel coordinates; `u`, `v`: the same scaled to 0..1.
  - `w`, `h`: image width and height.
- **Functions:** `sin cos tan asin acos atan atan2 sqrt pow exp ln log abs floor ceil`,
  `min max clamp(x,lo,hi) mix(a,b,t) step(edge,x) smoothstep(e0,e1,x) fract`, plus constants
  `pi` and `e`.
- **Examples:**
  - `g - (r + b) / 2`: how much greener than average (a foliage mask).
  - `step(0.5, fract(u * 10))`: vertical stripes.
  - `sin(y / 3) * 0.5 + 0.5`: scanlines.

**Image Expression**
- Three formulas, R, G and B, producing an image. The variables are the same as Expression.
- **Examples:** R = `g`, G = `r`, B = `b` swaps channels; R = `1 - r` inverts only red.

**Wavelength**
- The colour of light at a wavelength in nanometres (360..830). Connect a channel to sweep
  through the spectrum, for example a gradient to make a rainbow. See Colour Spaces.

**Blackbody**
- The colour of a hot object at a temperature in Kelvin (800..12000). Multiply an image by it with
  Blend > Multiply to warm or cool it like a white-balance change. See Colour Spaces.

**Normalize**
- Stretches a channel so its darkest pixel becomes 0 and its brightest 1. Useful after Math or
  Expression, when values have an unknown range.
- **Low % / High %:** use percentiles instead of the darkest and brightest pixel, so a few black
  or specular pixels don't set the range. For example 1 / 99 is an auto-exposure, and 0 / 99 makes
  brightness relative to the photo's highlights. Values outside the range go below 0 or above 1.

**Float Curve**
- A curve applied to a channel: the horizontal axis is the input, the vertical axis the output.
  Factor blends with the unchanged value.

**Set Alpha**
- **Replace Alpha:** replaces the image's alpha with a channel, for example a mask.
- **Apply Mask:** also multiplies the colour by it, so the result is transparent black outside
  the mask.

### Filter

Sizes are in full-resolution pixels.

**Blur**
- Gaussian-style blur. Size X and Size Y set the horizontal and vertical radius separately, so a
  large Size X with a small Size Y gives a horizontal smear.
- **Relative:** sizes become Factor X / Factor Y, percentages of the image width and height, so the
  same setting fits any photo resolution. **Aspect Correction Y** measures Y against the width too
  (a round blur on a non-square photo); **X** measures X against the height.

**Directional Blur**
- Motion-style blur with several optional parts:
  - **Distance and Angle:** a straight streak.
  - **Spin:** rotates around the centre, like a spinning camera.
  - **Zoom:** zooms from the centre, like a zoom-burst photo.
  - **Center X/Y:** the centre for Spin and Zoom, 0..1 across the image.
- **Quality:** High takes every sample. Fast builds the same streak by repeatedly averaging the
  image with a moved copy of itself, about 5x faster on large photos. It is slightly softer, and
  Zoom fades geometrically rather than linearly.

**Bilateral Blur**
- Smooths areas while keeping edges sharp (skin smoothing, noise reduction, painterly looks).
- Radius: how far to smooth. Color Sigma: how different colours may be and still be blended.
  Smaller values keep more edges.
- Determinator: an optional image whose edges are used instead of the input's own.
- **Quality:** High weighs a full square of neighbours. Fast blurs across, then down (about 4x
  faster on large photos); it can leave faint streaks along diagonal edges.

**Denoise**
- Removes noise from photos, with Lightroom's Detail-panel controls. Put it right after the
  Image Input, before Basic and sharpening.
- **Luminance:** smooths grain in brightness. 0 is off.
- **Detail:** higher keeps more fine texture (and some noise); lower smooths it more evenly.
- **Color:** removes coloured speckles and blotches without softening the image. About 25 suits
  most RAWs.
- **Color Detail:** higher keeps small coloured details (thin coloured edges); lower also removes
  large, low-frequency colour blotches.
- Works on wavelet bands, so edges stay sharp. Strengths are set for the full-resolution image,
  and the preview matches the export.

**Sharpen**
- Lightroom's Detail > Sharpening. It sharpens brightness only, so coloured edges get no
  fringes. Put it after Denoise and Basic.
- **Amount:** how strong. 40 is Lightroom's default for RAWs.
- **Radius:** the size of the details sharpened, in full-resolution pixels. 1 suits most photos;
  fine textures want less, soft images more.
- **Detail:** 0 keeps every edge within its neighbours' range, so no light or dark halos appear.
  Higher lets the full overshoot through and brings out fine texture.
- **Masking:** 0 sharpens everything. Higher protects flat areas (sky, skin) and sharpens only
  edges, which keeps noise from being sharpened. Feed a mask through Mix to limit it to a region.
- Like Lightroom's, judge it at 100%: the preview's proxy scales the radius down with the image.

**Capture Sharpening**
- RawTherapee's Capture Sharpening: undoes the slight blur every camera adds (lens, sensor
  filter, demosaicing) by deconvolution, which restores detail rather than adding contrast to
  edges as Sharpen does. Put it first, right after the Image Input of a RAW, before Denoise.
- **Radius:** the size of that blur, in full-resolution pixels (0.6 to 0.9 for most cameras;
  more for soft lenses). Too much gives haloes and a brittle look.
- **Iterations:** how far the deconvolution goes (20 is plenty; more for a larger radius).
- **Contrast Threshold:** areas flatter than this (sky, skin, noise) are left alone.
- **Amount:** blends it in; connect a mask to limit it to a region.
- It only shows at 100% and above: at the proxy's size the blur is below a pixel and the node
  passes the image through. It runs on the CPU.

**Diffuse or Sharpen**
- darktable's diffuse or sharpen, simplified: over a few **Iterations**, the image's detail at
  scales around **Radius** (full-resolution pixels; **Radius Span** widens the range of scales in
  octaves) is added back (positive **Amount**) or taken away (negative).
- Positive amounts sharpen, deblur a soft lens (Radius 1-4), or add local contrast and "clarity"
  (Radius 16-64). Negative amounts diffuse: a soft bloom or a surface blur (Radius 4-32).
- **Edge Sensitivity** slows the change down across strong edges: it keeps sharpening from making
  haloes and keeps edges crisp while diffusing (a smoothing that preserves edges). **Noise
  Threshold** stops the finest, faintest detail (noise) from being sharpened.
- More iterations make it smoother and stronger at the same Amount, and slower: it runs on the
  CPU, and large radii read far around each pixel.

**Spot Removal**
- Lightroom's Spot Removal: removes dust spots, blemishes and small distractions by covering
  each with pixels from somewhere else in the photo. Select the node and work on the Result panel.
- **Click** a blemish to add a spot. As in Lightroom, its source (the second circle, with an
  arrow pointing to the spot) is picked automatically: nearby texture that matches the spot's
  surroundings, clear of the spot and of other spots. Drag right after clicking, or drag the
  source circle later, to choose it yourself.
- **/** (or **Find New Source** in the Inspector) picks another automatic source for the selected
  spot, somewhere other than the current one.
- Drag a spot to move it and its edge to resize it. `[` / `]` change the Size, **Alt+click** or
  **Delete** removes a spot, and **Remove All** in the Inspector clears them.
- **Mode:** **Heal** copies the source's texture but matches the colour and brightness around the
  spot, so the patch blends in; **Clone** copies the source exactly (better next to hard edges).
- **Size** (a share of the image's long edge), **Feather** (how soft the edge is) and **Opacity**
  apply to the selected spot and to new ones.
- **Detect Dust** (in the Inspector) looks for sensor dust: small, round, soft spots a little
  darker than a smooth background such as sky. Each one it finds becomes a spot with an
  automatic source; spots already covered are skipped. Raise **Sensitivity** to find fainter
  dust, lower it if it marks real detail. Check the results, as Lightroom's Visualize Spots asks
  you to, and Alt+click any that aren't dust.
- Spots are kept in image-relative positions, so they stay in place at any preview size and in
  the export. Put the node early, before Basic and other adjustments.

**Remove**
- Content-aware fill (Lightroom's Remove without Generative AI, Photoshop's Content-Aware Fill):
  the area a **Mask** covers is rebuilt from patches of the rest of the photo, so texture such as
  grass, water or a wall carries on across it. For objects bigger than Spot Removal's circles:
  people in the background, signs, power lines.
- Paint the area with a **Brush Mask** (or any mask) and wire it to the Mask pin. Cover the whole
  object, shadow included.
- **Grow** (pixels) widens the area so the object's outline goes too. At 0 a soft mask edge
  blends the fill into the original.
- It works best with plenty of similar texture around the area; it can't invent what isn't
  anywhere in the photo (a face, the rest of a building). The result is the same every time.
  Large areas at full resolution take a few seconds.

**Filter**
- Classic 3x3 kernels:
  - **Soften:** a slight blur.
  - **Box Sharpen / Diamond Sharpen:** sharpening.
  - **Laplace:** thin edges.
  - **Sobel / Prewitt / Kirsch:** edge detection (bright lines on black).
  - **Shadow:** an embossed relief.
- Factor blends with the original.

**Dilate / Erode**
- Grows (positive Distance) or shrinks (negative Distance) a mask by that many pixels.
- **Distance** mode gives a hard edge; **Feather** gives a soft falloff.
- **Uses:** clean up keys, create outlines (dilated minus original), and choke edges.

**Kuwahara**
- An edge-preserving painterly filter that turns detail into flat brush-like patches. Size sets
  the brush size.

**Pixelate**
- Blocks of Size x Size pixels, each filled with its average colour.

**Posterize**
- Reduces each channel to Steps levels, for flat, poster-like colour bands. Connect a channel to
  Steps to vary it across the image.

**Glare**
- Adds bloom from bright areas. Only pixels above Threshold glow.
- **Type:**
  - **Fog Glow:** a soft halo.
  - **Streaks:** star-filter rays. Streaks sets the number of rays, Angle their rotation, and Fade
    how quickly they die away.
  - **Simple Star:** four thin rays.
- Size is the reach in pixels and Strength the brightness.
- Outputs the combined Image and the Glare alone, which you can colour or blend yourself.

**Sun Beams**
- Light rays radiating from a point (Source X/Y, 0..1). Bright areas are smeared away from the
  source. Length sets how far.

**Grain**
- Film grain, like Lightroom's Effects > Grain. It is monochrome and strongest in the mid-tones;
  pure black and white stay clean.
- **Amount:** how strong the grain is. Connect a mask to the Amount pin to vary it.
- **Size:** the grain's size, from 1 to 4 pixels of the full-resolution image. The
  preview shows grain smaller than its pixels as the export would look scaled down to the
  preview's size: weaker and finer. Zoom to 100% to judge it.
- **Roughness:** low is fine and even grain; high mixes in coarser clumps and an uneven density.
- **Seed:** another pattern of the same grain.
- Put it near the end of the graph, after sharpening and resizing would soften it.

**Vignette**
- Lightroom's Effects > Post-Crop Vignetting: darkens (negative **Amount**) or lightens
  (positive) towards the edges of the image it gets. After a Crop it follows the crop, unlike the
  Lens Correction's vignetting, which is measured on the whole frame. Amount -100 is two stops
  darker at the edges. Connect a mask to the Amount pin to vary it.
- **Style:** **Highlight Priority** works like exposure and keeps bright parts from clipping when
  lightening; **Color Priority** is a plain exposure change, which keeps the colours exactly;
  **Paint Overlay** blends towards black or white, as Lightroom's original vignette did.
- **Midpoint** sets how far in from the edges it reaches; **Roundness** goes from a rounded
  rectangle (-100) through an ellipse that fits the frame (0) to a circle (100); **Feather** sets
  how soft its edge is.
- **Highlights** (when darkening): bright parts such as lamps and sky keep their brightness in the
  darkened corners.

**Defringe**
- Lightroom's Lens Corrections > Defringe: removes purple and green fringes (longitudinal
  chromatic aberration) along high-contrast edges, such as branches against the sky or out of
  focus highlights. Pixels near an edge whose hue is in the range lose their colour, keeping their
  brightness.
- **Purple Amount** and **Green Amount** (0..20) set the strength and how far from the edge it
  reaches (up to 4 pixels). **Hue Low** and **High** narrow the hues it removes: the purple range
  goes from blue (0) to magenta (100), the green one from yellow-green to cyan-green. Narrow them
  if it removes colour from purple or green objects too.
- Lens Correction's and Lens Profile's chromatic aberration fix the other kind of fringe: red and
  cyan or blue and yellow edges that grow towards the corners.

### Transform

These move pixels around. Pixels pulled from outside the image are transparent, unless noted
otherwise.

**Transform**
- Moves (X/Y in pixels), rotates (Angle) and scales (Scale) the image around its centre.
- Wrap tiles the image instead of leaving transparent edges; it is good for seamless offsets and
  glitch shifts.

**Pan and Zoom**
- Moves and zooms the picture inside its own frame, for framing by eye. The output keeps the
  input's size, and wherever the moved picture doesn't reach is fully transparent.
- **Zoom** scales about the frame's centre (2 is twice as big); **X** and **Y** move it by
  fractions of the width and height (0.5 moves it half the frame right or down), so the preview
  matches the export.
- With the node selected, drag anywhere on the image in the viewer to move it, and Ctrl+wheel to
  zoom. The moved picture's outline is drawn over the viewer.
- **Interpolation:** Bilinear for smooth results, Nearest for hard pixels when zooming far in.

**Flip**
- Mirrors horizontally, vertically or both.

**Crop**
- Keeps the region between Left/Right and Top/Bottom (0..1 fractions of the image).
- Resize Image on: the output is only the cropped region. Off: the image keeps its size and the
  area outside the crop becomes transparent.
- **Angle** straightens the image first (positive turns it clockwise). With **Constrain to
  Image** on, it is scaled up just enough that no empty corners show.
- **Aspect** locks the crop to a ratio (Original, 1:1, 4:5, 2:3, 16:9 and more). The rectangle
  shrinks around its centre to fit.
- Select the node to edit the crop on the Result panel (see On-image Controls).

**Border**
- Puts a coloured margin around the image, as darktable's framing or a print border: the output
  is bigger than the input. Sizes are percentages of the image's long edge.
- **Size:** the margin on every side. **Bottom:** extra margin below, for a gallery or instant
  photo look.
- **Aspect** grows the canvas (never cropping the picture) to a ratio, such as 1:1 for social
  media or 4:5 for a print, with the picture centred.
- **Color:** the margin's colour; transparent parts of the picture show it too. **Line Size** and
  **Line Color** draw a thin frame line around the picture.

**Watermark**
- Lightroom's export watermark: a line of text or a logo, drawn over the picture.
- **Text** (empty for none) in **Font** Sans, Sans Bold, Serif or Monospace, using the Windows
  fonts (Segoe UI or Arial, Georgia or Times, Consolas or Courier). Without them it uses the
  built-in Roboto (Cousine for Monospace).
- **Size** is the line height as a percentage of the picture's short edge, so it looks the same
  at any export size. A **Logo** image connected to its pin is drawn instead of the text, two and
  a half times the text's height, keeping its transparency.
- **Anchor** picks one of nine positions (corners, edges, centre); **Inset X/Y** move it in from
  the edges, as percentages of the short edge.
- **Opacity**, **Color**, and **Shadow** (a soft dark shadow that keeps light text readable on
  bright areas).
- Put it after Border to sign the margin, or last before File Output.

**Lens Distortion**
- Distortion: positive for barrel (bulging), negative for pincushion.
- Dispersion: shifts red and blue apart towards the edges for chromatic aberration, a great
  vintage or glitch detail.
- Fit scales the image so no empty corners show.

**Lens Correction**
- Lightroom's manual lens corrections.
- **Distortion:** positive straightens barrel distortion (lines bowing outwards), negative
  straightens pincushion. Constrain to Image scales up to hide the empty edges.
- **Red / Cyan** and **Blue / Yellow** shift those channels radially to remove (or add) colour
  fringes along high-contrast edges near the corners.
- **Vignetting** brightens (positive) or darkens (negative) the corners; **Midpoint** sets how far
  in the effect reaches.

**Lens Profile**
- Lightroom's Enable Profile Corrections: undoes the distortion, colour fringing (lateral
  chromatic aberration) and vignetting measured for your lens, at the photo's focal length and
  aperture. The profiles come from [lensfun](https://lensfun.github.io)'s database (CC BY-SA 3.0),
  which isn't part of NodeLab.exe: the Inspector offers to download it (about 3 MB, into
  `%APPDATA%\NodeLab\lensfun`).
- The node reads the camera, lens, focal length and aperture from the EXIF of the Image Input
  feeding it, and picks the best-matching profile when you first select it. **Detect from Photo**
  looks again; **Choose Lens** picks one by hand (for manual lenses, which write no EXIF).
- The chosen profile is saved in the project, so it renders the same on another computer without
  the database.
- **Distortion** and **Vignetting** set how much of the correction to apply (100 is the profile's,
  200 twice as much). **Chromatic Aberration** turns the fringe correction on or off.
  **Constrain Crop** zooms in to hide the empty edges a distortion correction leaves.
- Put it first, before anything that crops, rotates or blurs: profiles are measured on the whole
  frame as it came from the lens.

**Perspective**
- Lightroom's Transform panel: fixes converging verticals and tilted horizons as if the camera had
  been held level (a true perspective change, not a stretch).
- **Upright: Guided.** Select the node and drag on the Result panel to draw up to four guides
  along lines that should be straight: steep ones become vertical, flat ones horizontal. Two
  guides along a building's sides straighten it; add two along a roof line or the horizon to level
  it as well. Drag a guide's ends to adjust it; Alt+click a guide to remove it. Off ignores the
  guides (they're kept).
- **Upright: Level, Vertical, Full, Auto.** NodeLab finds straight edges in the photo itself (those
  within 20 degrees of vertical or horizontal) and turns the camera to straighten them, ignoring
  lines that disagree with the rest, such as a sloping roof. **Level** only rotates, to level the
  horizon; **Vertical** also stands converging verticals up; **Full** corrects both ways;
  **Auto** is a gentler Full (the sideways correction at half strength, at most 20 degrees). The
  sliders apply on top. Turn on Constrain Crop to hide the empty corners.
- **Vertical** below zero widens the top, for buildings leaning back when shot looking up; above
  zero widens the bottom. **Horizontal** does the same sideways (positive widens the right side).
- **Rotate** turns the image clockwise (positive), up to 10 degrees. **Aspect** stretches it
  wider (positive) or taller (negative), to restore proportions after a strong correction.
- **Scale** zooms in or out, **X Offset** and **Y Offset** move the image (positive moves right
  and up). **Constrain Crop** zooms in just enough that no empty corners show.
- A fine grid shows over the Result panel while the node is selected, to line things up against.

**Displace**
- Pushes each pixel sideways by the X channel and up and down by the Y channel. 0.5 means no
  movement, 0 moves fully one way and 1 the other, scaled by Strength X/Y in pixels.
- **Uses:** feed it a Noise Texture for heat-haze or water ripples, or a Wave Texture for wobbles.

**Map UV**
- Re-maps an image through a UV image: the UV image's red channel says which column (0..1) to
  sample and green which row.
- The output has the UV image's size. Advanced; mostly used with Expression or Gradient-built UV
  images.

**Corner Pin**
- Pins the image's four corners to new positions (0..1 fractions), with true perspective. Use it
  to place an image onto a screen or sign, or to straighten a photographed document.

### Matte

Masks and keyers. A matte is a channel where 1 = keep and 0 = remove. Keyers also output the Image
with the matte applied as alpha.

**Box Mask / Ellipse Mask**
- A rectangle or ellipse: X/Y centre, Width/Height and Rotation, in 0..1 fractions of the image.
- Feather softens the edge; Value is the brightness inside.
- Operation combines it with the incoming Mask: Add (union), Subtract, Multiply (intersection), or
  Not (everything outside the shape).
- Chain several masks for complex shapes, or use as a vignette with Mix.
- Select the node to drag its handles on the Result panel.

**Radial Gradient**
- Like Ellipse Mask, but soft by default (Feather 0.5), like Lightroom's Radial Gradient. It is
  the quickest way to brighten a subject or make a custom vignette (Operation Not).

**Linear Gradient**
- Fully on at Start and fading to nothing at End (points as 0..1 fractions of the image), like
  Lightroom's Linear Gradient. Drag the handles on the Result panel. In scene-linear projects
  the fade is even across the whole Start-End span (older sRGB projects keep a smoother S-shaped
  fade, so they render as they were made).
- **Uses:** darken a sky (Start at the top, End at the horizon) by wiring it into a Basic node's
  Factor.

**Brush Mask**
- Paint a mask by hand on the Result panel while the node is selected: left mouse paints,
  Alt+paint erases, and `[` / `]` change the size.
- Size is a fraction of the image's long edge. Feather softens the brush edge, and Flow is how much
  each stroke adds. New strokes use the current settings.
- The Inspector shows the stroke count, with **Remove Last** and **Clear Strokes** buttons.
  Strokes are saved in the project and scale with the image.
- An incoming Mask is painted over; Invert flips the result.
- **Auto Mask** (Lightroom's option): new strokes paint only over colours like the one under the
  brush, so you can brush up to an edge (sky against a roofline, a face against hair). Wire the
  photo into the node's **Image** input; **Add Mask** does this for you. The colour is taken
  under each point of the stroke, so one stroke can follow a surface as its colour changes.
  Turning it off affects only new strokes.

**Range Mask**
- Selects part of the photo by brightness or colour, like Lightroom's Luminance Range and Color
  Range. Wire a gradient or brush mask into **Mask** to refine it: the result keeps only the
  parts of that mask in range. With nothing in Mask it selects across the whole image.
- **Luminance:** keeps lightness between **Low** and **High** (0 black, 0.5 a mid tone, 1 white,
  in perceptual lightness whatever the working space). **Smoothness** feathers both ends.
- **Color:** pick the **Color** with the eyedropper; **Amount** widens how different a colour can
  be and still be selected.
- **Invert** selects everything else instead.

**HSL Mask**
- Selects colours by hue, saturation and lightness together, like DaVinci Resolve's HSL
  qualifier: "the saturated blues", "the light greens". Each part has a **Use** switch, a range
  and a **Softness** that feathers its edges. Like Range Mask, wire a mask into **Mask** to keep
  only the part of it in range.
- **Hue** is the centre in degrees (0 red, 60 yellow, 120 green, 180 cyan, 240 blue, 300
  magenta) and **Hue Width** how far around it to take, in all. Grey pixels have no hue, so a
  hue range leaves them out.
- **Saturation** runs from 0 (grey) to 1 (the most vivid sRGB colours); the default **Low** of
  0.15 leaves out near-greys. **Lightness** is perceptual, as in Range Mask.
- Measured in Oklch, so a range is the same width to the eye in every colour.
- **Invert** selects everything else instead.

**Select Subject**
- Lightroom's Select Subject: an AI model finds the main subject of the photo (people, animals,
  objects in front) and the node outputs it as a mask. Wire the photo into **Image**.
- The model isn't part of NodeLab.exe. The first time, the Inspector offers to download it
  (about 240 MB with the AI runtime, once; stored in `%APPDATA%\NodeLab\models`). Until then the
  mask is empty. `NodeLab.exe --install-model subject` downloads it from the command line.
- The model sees a 1024 x 1024 copy of the photo, so it runs once per picture, not per pixel:
  about a minute on a laptop CPU, a second or two on a good GPU (Edit > Preferences >
  Compositor > AI Masks). Results are cached by picture, also on disk, so the export, the zoomed
  views and reopening the project reuse it. Edits upstream that only change tones (exposure,
  white balance, contrast) keep the mask; a crop or a retouch runs the model again.
- While editing, the model runs in the background: everything else keeps updating, and the mask
  stays empty (or shows the previous picture's) until it is done. The Inspector and the status
  bar show its progress. File > Export and `--render` wait for it.
- While it runs, the model needs about 4 GB of memory on the CPU; it is unloaded as soon as it
  finishes. On a computer with 8 GB, close other big programs if it is slow.
- **Refine Edges** fits the mask's edges to the full-resolution photo (a guided filter), so hair
  and outlines stay crisp at any size. **Invert** selects the background instead (Add Mask >
  Background). An incoming **Mask** limits the result to it.
- **Model:** **Accurate** (BiRefNet, above) or **Light** (U²-Net small): a 4.6 MB download that
  runs in about a second with little memory, for computers where Accurate is too slow. Its mask
  is coarser (it sees a 320 x 320 copy), so keep **Refine Edges** on.
  `NodeLab.exe --install-model subject-light` downloads it from the command line.
- Run it on the photo itself, before colour swaps or heavy grading: the model was trained on
  ordinary photos. For an infrared edit, wire the image from before the channel swap.
- **Feather** (all AI masks) softens the mask's outline, up to 3% of the long edge at 100.
  **Edge** (Lightroom's Shift Edge) grows the mask outwards (above 0) or shrinks it inwards
  (below 0), by up to 2% of the long edge, for a halo the model left or a background it took.

**Select Sky**
- Lightroom's Select Sky: an AI model (a U²-Net trained on skies) masks the sky, up to the
  horizon, trees and rooftops. It works like **Select Subject**: a one-time download (about
  190 MB, or 175 MB once the runtime is there), the same caching, **Refine Edges** for crisp
  edges at full resolution, **Invert** for everything but the sky, and an optional **Mask**.
- It runs in a second or two on the CPU. Typical use: Add Mask > Sky, then lower the Basic's
  Exposure or Highlights, or raise Dehaze, for the sky only.

**Select People**
- Lightroom's Select People for portraits: an AI face-parsing model (SegFormer, trained on
  CelebAMask-HQ) masks parts of a face. Tick the parts to include: **Face Skin** (with nose and
  ears), **Eyebrows**, **Eyes**, **Lips**, **Mouth** (inside, teeth), **Hair**, **Neck**,
  **Clothes** and **Accessories** (glasses, hat, earrings, necklace).
- Typical use: Face Skin into a Basic with a little negative Texture for softer skin, Eyes for a
  touch of Exposure and Clarity, Lips for Saturation.
- The model was trained on close-up faces, so it works when a face fills much of the frame (a
  head-and-shoulders portrait). For a full-length or group shot, crop to the face first or use
  **Select Subject**. It doesn't tell people apart: wire a shape mask to **Mask** to keep one.
- Like the other AI masks it is a one-time download (about 340 MB) with the same caching,
  **Refine Edges**, **Invert**, **Feather** and **Edge**. It takes a few seconds on the CPU.
  Changing which parts are ticked runs the model again (each choice is cached).
  `NodeLab.exe --install-model face` downloads it from the command line.
- The model's licence (NVIDIA's for SegFormer, and the CelebAMask-HQ data's) allows
  non-commercial use only.

**Select Landscape**
- Lightroom's Select Landscape: an AI scene model (SegFormer-B2, trained on ADE20K's 150 kinds of
  things and places) masks parts of a scene. Tick **Sky**, **Water** (sea, lakes, rivers,
  waterfalls), **Vegetation** (trees, grass, plants, fields), **Mountains** (and hills),
  **Natural Ground** (earth, rock, sand), **Architecture** (buildings, walls, bridges, fences)
  and **Artificial Ground** (roads, paths, floors, stairs). Snow has no class of its own in the
  model; try Natural Ground or Mountains, or a Range Mask on bright areas.
- A one-time download of about 110 MB (shared with **Select Objects**), a second or two on the
  CPU. Otherwise as **Select Subject**: caching, **Refine Edges**, **Invert**, **Feather**,
  **Edge** and an optional **Mask**. `NodeLab.exe --install-model scene` downloads it.
- The model's licence (NVIDIA's for SegFormer) allows non-commercial use only.

**Select Objects**
- Masks kinds of objects with the same scene model as **Select Landscape**: **People**,
  **Animals**, **Vehicles**, **Furniture**, **Signs and Poles**, **Lights**, **Screens** and
  **Potted Plants and Flowers**. Lightroom's Select Objects asks you to brush over one object;
  here, wire a Brush Mask (or a Box or Ellipse Mask) to **Mask** to keep only the one you want.
- Useful with **Remove**: Select Objects with Signs and Poles, a Box Mask around the sign, and
  Remove fills it in.

**Channel Key**
- Keys on a single channel: Red, Green, Blue, Hue, Saturation, Value, Y (luma), Cb or Cr.
- Pixels at or below Low are kept; pixels at or above High are removed, with a soft transition
  between. Invert flips it.

**Luminance Key**
- Keys by brightness between Low and High. Keep Bright chooses whether the bright or the dark
  side is kept.
- **Uses:** isolating skies and highlights, or dark silhouettes.

**Difference Key**
- Removes pixels close to a key colour, using the largest per-channel difference.
- Key Color is the colour to remove, or connect an image to Key for a clean-plate difference
  (removes everything that matches a background shot).
- Tolerance: how close counts as a match. Falloff: the soft edge width.

**Distance Key**
- Like Difference Key but measures the straight-line colour distance.
- Space: RGB, or YCbCr (ignore brightness), which ignores shadows falling on the key colour.

**Chroma Key**
- Green-screen style: compares hue angles in the CbCr colour plane.
- Acceptance is the angle (degrees) around the key hue that is removed; Falloff softens the edge.
- Min Saturation keeps grayish pixels, which have no reliable hue.

**Color Spill**
- Removes the coloured cast a green (or blue) screen throws onto the subject.
- Spill Channel: the screen's colour.
- Limit: compare against the next channel (Single) or the average of the other two.
- Ratio: how strictly to limit. Factor blends with the original.

**Double Edge Mask**
- Creates a gradient between an Inner Mask (value 1) and an Outer Mask (value 0).
- **Uses:** soft, custom-shaped edges, for example a hand-shaped vignette.

### Texture

Textures generate patterns from nothing. They take no image and produce the project's working size.
Coordinates are relative to the image, so textures look the same in the preview and the export.

**Noise Texture**
- Smooth, cloudy Perlin noise (fBm).
  - **Scale:** feature size (higher is finer).
  - **Detail:** the number of layers of finer noise.
  - **Roughness:** how strong the fine layers are.
  - **Lacunarity:** the size jump between layers.
  - **Distortion:** warps the noise with itself.
  - **Seed:** a different pattern.
- Outputs Fac (grayscale) and Color (three independent noises as RGB).

**Voronoi Texture**
- A cell pattern. Distance is the distance to the nearest cell centre (dark at the centres); Color
  is a random colour per cell.
- Randomness at 0 gives a regular grid.
- **Uses:** stained glass, cracked earth, and cell-based glitch masks.

**Gradient Texture**
- Ramps from 0 to 1: Linear, Quadratic, Easing, Diagonal, Spherical, Quadratic Sphere or Radial.
  Angle rotates it.
- **Uses:** gradient-map looks with Color Ramp, graduated filters with Mix, and radial vignettes.

**Wave Texture**
- Bands or Rings with a Sine, Saw or Triangle profile.
- Distortion (and Detail) makes them wobble like wood grain or marble. Phase shifts the waves.

**Checker Texture**
- A checkerboard of Color 1 and Color 2 at Scale squares across.

**White Noise**
- Random values per pixel (or per block of Grain Size pixels).
- **Uses:** dithering, digital static and blocky grain (blend with Overlay or Add at low Factor). For
  film grain, use the Grain node.

### Utility

**Reroute**
- A small node that passes a wire through, to tidy up the layout. Shift+right-drag across a wire
  adds one. Drag its bar to move it, as any node; its pins are on the ends.

**Switch**
- Passes On or Off through, depending on the On checkbox. Good for A/B testing two branches.

**Split (Compare)**
- Shows A on one side of a line and B on the other: before/after comparisons.
- Position moves the line; Orientation is a Vertical or Horizontal line; Show Line draws it.

**Image Info**
- Outputs the image's Width, Height and Aspect ratio as Numbers.

**File Output**
- Saves whatever is connected to a file (PNG, JPEG, TIFF, OpenEXR, WebP, JPEG XL or AVIF) at full resolution when you
  export a single image or choose File > Write File Outputs, or when rendering from the command
  line. Batch exports skip it, because every image would overwrite the same file.
- **Format** follows the file type picked in Browse. Only the chosen format's setting is shown:
  **Color Depth** (8 or 16 bit; 16 means 10 for AVIF) for PNG, TIFF, JPEG XL and AVIF, **EXR
  Depth** (Float (Half) or Float (Full)) for OpenEXR, **Lossless** for WebP, JPEG XL and AVIF,
  and **Quality** for JPEG and the lossy ones. See Exporting for what each format stores.
- Use several to export multiple versions (for example colour and black-and-white) in one go.
  Enabled switches one off without deleting it.

### Groups

**Group**
- Select nodes and press **Ctrl+G** to pack them into one Group node. Wires crossing the
  selection's border become the group's inputs and outputs.
- **Tab** enters the selected group, and **Tab** again (with nothing selected) goes back out. The
  breadcrumb above the graph shows where you are.
- Inside, the Group Input and Group Output nodes are the group's pins. Rename, reorder, add, or
  change their type in the Inspector.
- **Input values:** like Blender's group sockets, a Channel or Number input has a Default, Min and
  Max (set in the Inspector under the pin). While it is unconnected, the group node shows it as a
  slider, so a group works like a node with its own settings.
  - Grouping takes the range and value from the slider of the pin the input feeds, so a grouped
    graph renders the same.
  - Image inputs have no value: unconnected, they are empty.
- **Ctrl+Alt+G** ungroups. Groups can be nested.

**Value Input**
- One of the group's input sockets as a node of its own, to place wherever the value is used
  instead of wiring everything from the Group Input node. Only offered inside a group: Add menu >
  Group.
- Adding one adds a Number input to the group (a slider on the group node). Rename it with F2 or
  in the Inspector, which also sets its Default, Min and Max, and its Type: Number, Channel (a
  mask) or Image. Several Value Input nodes can read the same socket (duplicate one).
- Deleting the last Value Input of a socket it made removes the socket again, unless the Group
  Input node still uses it.

**Value Output**
- One of the group's output sockets as a node: what is wired into it comes out of the group.
  Adding one adds a Number output; its Type in the Inspector switches it to Channel or Image.
- If the Group Output node's pin for the same socket is wired too, that wire wins.

### Presets

- Right-click a node (usually a group with its sliders) and choose **Save as Preset...** to keep
  the selected nodes under a name. **Add > Presets** inserts them into any project, and
  Add > Presets > Delete Preset removes one.
- Presets are files in `%APPDATA%\NodeLab\presets` (`.nlpreset`), which can be copied to another
  computer.

## Working in the Node Editor

| Action | How |
|---|---|
| Add a node | Right-click empty space, or press Shift+A. Type to search, then press Enter or click, or browse the menus. Over a menu name or its list, the wheel steps through its nodes (Enter adds the highlighted one). |
| Connect | Drag from an output pin to an input pin (or the other way round). |
| Add a connected node | Drag a wire into empty space and pick a node from the menu. |
| Insert into a wire | Drag a node onto a wire; it is connected when you release. |
| Swap | Shift+S (or right-click > Swap...) replaces the selected nodes with another type, like Blender's Swap. Wires stay connected where the new node has a matching pin, and settings with the same name are kept. |
| Arrange | Shift+P (or Edit > Arrange Nodes) lays the selected nodes, or all nodes when fewer than two are selected, out in columns from left to right. |
| Pull out of a chain | Alt+drag the node. |
| Disconnect | Drag a wire off an input pin and drop it on empty space, or Ctrl+right-drag across wires (knife). |
| Pan / zoom | Drag empty space or middle-drag; mouse wheel zooms. |
| Frame all / selected | Home / . (period) |
| Find a node | Ctrl+F, or View > Find Node... Type part of a label or node name; Enter (or a click) selects the node and frames it. Labelled nodes are listed first. Nodes inside groups are listed too, as "Group > Node", and picking one opens that group. |
| Select | Click; Shift+click adds; Shift+drag box-selects; Ctrl+A selects all. |
| Select linked | L (upstream) / Shift+L (downstream) |
| Move | Drag, or G then move the mouse and click. |
| Duplicate | Ctrl+D, or Shift+D to duplicate and move. |
| Copy / paste | Ctrl+C / Ctrl+V (works between projects) |
| Delete | Delete or X reconnects the wires around the node; Alt+Delete doesn't. |
| Mute | M: the node passes its input straight through (shown with a red line). |
| Collapse | H: hides the node's settings. |
| Rename | F2, or right-click > Rename. |
| Edit a value | Double-click a slider in the Inspector (or click a node's value box) to type a number. Backspace over one resets it to its default, and right-click offers Reset to Default and Edit Value. |
| Reset to defaults | Right-click > Reset to Defaults (or Edit > Reset to Defaults) puts every setting of the selected nodes back to its default. File paths are kept, and a RAW gets its RAW defaults again. |
| Make links | F connects the selected nodes in a chain, left to right (top to bottom in a column). Nodes already wired together are skipped, and when every matching input is taken, F replaces one. Afterwards a hint at the bottom of the canvas names the new wire: press 1 for the next output, 2 for the next input, or F again for the next pair (taken inputs are skipped, and an input F replaced gets its wire back). Esc, a click or a new selection ends it. |
| Detach links | Alt+D (or right-click > Detach Links) removes the wires between the selected nodes. Their wires to unselected nodes stay. To take a node out of a chain instead, Alt+drag it. |
| Swap links | Alt+S (or right-click > Swap Links), as in Node Wrangler. With two nodes selected they trade places, wires and position. With one, its two wired inputs trade wires (a Mix's A and B), or its one wire moves to the next input that takes it. |
| Reroute | Shift+right-drag across wires. |
| Preview | Ctrl+click a node; Ctrl+Shift+click for its next output. |
| Group / ungroup / enter | Ctrl+G / Ctrl+Alt+G / Tab |
| Frame | Ctrl+J frames the selection. Drag the title to move it with its nodes and the corner to resize. Double-click the title to rename it; right-click it for colour and options. |
| Move nodes between frames | Right-click a node > Move to Frame, or right-click a frame title > Move Selected Nodes Here. Alt+P removes nodes from their frame. |
| Undo / redo | Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) |
| Guide | F1 opens this guide at the selected node's entry. |
| View an intermediate node | Right-click it > Open in New Viewer. |
| Pick a colour | Pick next to a colour setting, then click or drag on an image. |

New, pasted, swapped and spliced nodes push any nodes they land on out of the way, so the graph
doesn't pile up. Sideways pushes take the pushed node's downstream (or upstream) chain along so
wires keep flowing left to right.

In the image panels, drag to pan and scroll to zoom. The Original and Result panels move together.
Double-click to reset the view. Zoomed in past the preview, they sharpen to full resolution once
the view stops moving.

## Exporting

**File > Export** (Ctrl+E) opens the Export window. The render runs in the background, so you can
keep working (later edits don't affect a running export); the status bar and the window show
progress, and **Cancel** stops it.

- **Single** renders the Output node to one file. It also writes the File Output nodes unless you
  untick that.
- **Batch** runs many photos through the same node tree. Add sources with **Add Files...**,
  **Add Folder...**, or by dropping images or folders on the window while the Batch tab is open.
  Choose which Image Input receives them (when the tree has several) and an output folder. Each
  result is named by the **File Naming** template (below). Sizes in pixels (blur radius, offsets)
  apply to every photo, and each result keeps its own source's size.
- **Preset** (Lightroom's export presets): pick **Full-Size JPEG**, **Web JPEG (2048 px)**,
  **Email (1000 px)**, **Full-Size PNG**, **16-bit TIFF** or **OpenEXR (Half Float)** to set the
  format, size, sharpening and naming in one go. **Save...** keeps the current settings as your
  own preset (in the preferences, so every project has it); **Delete** removes one of yours.
- **Also export** (darktable's multi-preset export): tick presets to write each image with them
  too, from the same render, so a full-size TIFF and a web JPEG come out of one export. Each
  preset's files go into a subfolder named after it (next to the single export's file, or in the
  batch's output folder), named by that preset's own template. It applies to Export Selected in
  the Library as well, and is remembered in the preferences.
- **File Naming** (Lightroom's filename templates), for batches and Export Selected: text with
  tokens in braces, which the **Insert** menu adds, and an example name below it. `{name}` is the
  source's file name, `{folder}` its folder, `{seq}` the position in the export (`{seq:3}` pads it
  to 001), `{copy}` "Copy 1" for a virtual copy, `{date}` the capture date (YYYY-MM-DD) with
  `{year}` `{month}` `{day}` `{time}` and the others, `{today}` the export date, and `{camera}`,
  `{lens}`, `{iso}`, `{focal}`, `{aperture}` and `{shutter}` from the EXIF. The default is
  `{name}_edit`. Two photos that would get the same name get " (2)", " (3)" added, so no export
  overwrites another, and an export never overwrites its source.
- **Format:**
  - **PNG** and **TIFF** (8 or 16 bit), **JPEG** (with a quality setting), **WebP** (8 bit),
    **JPEG XL** (8 or 16 bit) and **AVIF** (8 or 10 bit) are display images: the view transform
    is applied, as in the viewer, and they are tagged with their colour space (PNG's sRGB chunk
    or an ICC profile, an embedded ICC profile in the others, or AVIF's colour tag for sRGB), so
    other apps show the same colours.
  - 16 bit keeps smooth gradients for further editing elsewhere; 8 bit is for sharing.
  - **WebP**, **JPEG XL** and **AVIF** are **Lossless** (every pixel kept exactly) or, with
    Lossless unticked, lossy at a **Quality** like JPEG's.
    - Lossless WebP is usually about a third smaller than PNG; lossy WebP is the web's
      everyday format, smaller than JPEG at the same quality.
    - **JPEG XL** gives the smallest files for its quality, keeps 16 bit, and can be lossless
      far smaller than PNG or TIFF. Lightroom, Photoshop, macOS, iOS and Safari read it; Chrome
      and Windows Photos need an extension.
    - **AVIF** is what browsers and phones prefer for small HDR and wide-gamut pictures; its
      lossless mode is larger than JPEG XL's. Quality 90 and above keeps full colour
      resolution; below it, colour is stored at half resolution, as JPEG does.
    - JPEG XL is slow to encode at 16 bit and large sizes: allow a few seconds per photo.
  - **Color Space** (Lightroom's): **sRGB** for the web and anything unsure; **Display P3** for
    Apple devices and modern phones and screens; **Adobe RGB (1998)** for print workflows;
    **ProPhoto RGB** (use 16 bit) to hand the widest colours to another editor; **Rec.2020**.
    In a scene-linear project with the Standard view, the wider spaces keep the saturated
    colours sRGB would clip. Other views and legacy projects make sRGB colours, which are
    converted unchanged. Use **Gamut** in the Result toolbar to see what a space clips.
  - **Rec.2100 PQ (HDR)** writes an HDR PNG (16 bit, with the cICP chunk that browsers and HDR
    displays read), JPEG XL (16 bit) or AVIF (10 bit): scene-linear white is shown at 203 nits
    and brighter values stay brighter, up to 10,000 nits, instead of clipping. Exposure applies;
    the view transform doesn't. Other formats get Rec.2020 instead.
  - **OpenEXR** (Float (Half) or Float (Full)) keeps the scene-linear values without the view
    transform, as Blender does, including values above 1. Use it to hand the image to another
    compositor or grading tool. Half is about a third the size of Full and is plenty for photos.
  - **JPEG**, **WebP**, **JPEG XL** and **AVIF** keep the source photo's EXIF data (camera, lens,
    exposure, date), from JPEG and camera RAW sources. The orientation is reset, because the pixels are already upright.
  - A library photo's title, caption, keywords, rating and colour label are written as XMP into
    PNG, JPEG, TIFF, WebP, JPEG XL and AVIF files (not EXR), where Lightroom, Bridge and file
    browsers read them.
  - Alpha is dropped when the image is fully opaque.
- **Size:** Original, **Long edge** (pixels), or **Percent**. Exports are only ever made smaller,
  with a sharp Lanczos filter in linear light (the same as Blender's and darktable's high-quality
  resize), so fine detail stays crisp without halos.
- **Output Sharpening** (Lightroom's): **Sharpen for Screen**, **Matte Paper** or **Glossy
  Paper**, at **Low**, **Standard** or **High**. It is applied after resizing, so it suits the
  final pixel size; use Screen for web and phone images and the paper options for prints.

The settings and folders are saved with the project. From the command line,
`NodeLab.exe --batch project.nlproj outDir [--png|--jpg|--tif|--exr|--webp|--jxl|--avif] [--depth N] [--quality Q] a.jpg b.jpg ...`
does the same batch without the window, and `NodeLab.exe --render project.nlproj out.tif
[--depth 16] [--quality Q]` renders one image (the extension picks the format; `--depth 32` for
full-float EXR; `--quality` makes WebP, JPEG XL and AVIF lossy at that quality).
Add `--device gpu` to render on the graphics card (`--precision half` for speed, full by default).

## Snapshots

**View > Snapshots** opens Lightroom's Snapshots panel: named states of the whole edit, to come
back to or compare.

- **Create Snapshot** stores the current node tree, named after the date and time; type a new name.
- Click a snapshot to **Restore** it (it can be undone with Ctrl+Z). Right-click for **Rename**,
  **Update with Current Settings** and **Delete**.
- Snapshots are saved with the project (and with a library photo's sidecar), so they survive
  closing NodeLab, as is the History below; a snapshot is a state you name and keep on purpose.

## History

**View > History** opens Lightroom's History panel: every undo step by name, the newest at the
top, such as **Add Curves**, **Basic: Exposure 0.50**, **Connect** or **Mute Blur**. A change
inside a group is named after the group (**Look: Add Invert**).

- Click a step to go back to it; the later steps stay in the list, greyed, until you change
  something (as with Ctrl+Z and Ctrl+Y), and clicking one goes forward again.
- The history (up to 200 steps) is saved with the project and with a library photo's sidecar,
  as darktable keeps its history stack, so Ctrl+Z still works after reopening. Each step is
  stored as the change from the next one, so it adds little to the file. If the project was
  changed outside NodeLab (or by a version that saves it differently), the old steps are dropped.
- **Clear History** forgets the steps and keeps the edit (saved at the next save).

## Preferences

**Edit > Preferences** holds the settings that belong to you rather than to a project. They are
saved straight away, in `%APPDATA%\NodeLab\preferences.json`.

- **Interface:** the layout preset, whether the Inspector is an overlay or a panel, and node
  timings.
- **Themes:** NodeLab Dark (the default), Blender, Darkroom (neutral greys that don't tint how a
  photo is judged), Midnight, High Contrast and Light. Every colour can be changed: the interface's
  background, title bars, widgets, text, accent and borders, and the Node Editor's canvas, nodes,
  fields, wires and the header colour of each node category. Editing a built-in theme makes a
  **Custom** one; type a name and choose **Save Theme** to keep it in the list.
- **Viewer:** the colour behind images.
- **Compositor:** the Device (CPU or GPU) and Precision, as in View > Compositor.
- **New Projects:** the view transform (Standard or AgX, with a look) new projects and library
  photos start with. RAW photos always start with AgX.
- **Save & Load:** **Auto Save** (on by default) and its **Timer** (5 minutes by default). Unsaved
  changes are saved that long after the first one, in place. Only a project that has been saved
  once, or a library photo's edit, is auto saved; an untitled project waits for its first Save.

## Library

**File > Open Folder** (Ctrl+Shift+O) turns NodeLab into a photo browser, like Lightroom's
filmstrip. You can also drop a folder on the window, or start `NodeLab.exe C:\Photos\Trip`.
The Library panel along the bottom shows the folder's photos.

- **Opening photos:** click a thumbnail, or press ← / → to step through them. The open photo
  has a white border.
- **Edits are saved automatically** in a sidecar next to each photo: `IMG_1234.CR3` gets
  `IMG_1234.CR3.nlproj`. Switching photos saves the one you leave, with no question asked, so
  the edits travel with the photos. A sidecar is an ordinary project, so File > Open Project
  opens it too. Ctrl+S saves the sidecar straight away; Ctrl+Z works within each photo.
- **New photos** start with Image Input → Denoise → Basic → Output, in scene-linear. RAWs get
  Denoise's Color at 25 (Lightroom's default colour noise reduction) and the AgX view; other
  photos start with Denoise off.
- **Rating and flags** (Lightroom's keys, with the mouse anywhere but the Node Editor):
  - **0-5** rate the selected photos (shown as dots under the thumbnail).
  - **P** picks (a white flag), **X** rejects (a red cross, and the thumbnail dims), **U**
    clears the flag.
  - **6-9** give the **colour labels** Red, Yellow, Green and Blue (a bar along the bottom of the
    thumbnail); the same key again removes it. Purple is in right-click > Color Label.
  - Rating a photo writes its sidecar, so ratings, flags, labels and keywords stay with the folder.
- **Selecting:** Ctrl+click adds or removes a photo, Shift+click selects a range. Selected
  photos have a blue border.
- **Filter:** the menu shows All Photos, Picked, everything but rejects, Rejected, photos with at
  least N stars, Edited photos, one colour label, or Duplicates. ← / → skip photos the filter hides.
- **Search** (the box on the Library toolbar): shows the photos whose file name, title, caption or
  keywords contain every word typed, ignoring case.
- **Metadata** (the toolbar's **Metadata** button, or View > Metadata), as Lightroom's Metadata and
  Keywording panels: the colour label, **Title**, **Caption** and **Keywords** of the selected
  photos, and the camera's details for one photo.
  - With several photos selected, a field they don't share shows "(mixed)"; typing replaces it in
    all of them. A text field applies when you leave it (Enter, Tab or a click elsewhere).
  - Type keywords separated by commas and press Enter to add them. Click a keyword's button to
    remove it from the selection (the number is how many of the selected photos have it).
  - **Keyword List** has every keyword in the folder with its count: click one to add it.
  - In the Library views it is a sidebar on the right; over the editor it is a window.
- **Collections** (the toolbar's **Collections** menu, as Lightroom's): named lists of photos from
  any folders, kept in `%APPDATA%\NodeLab\collections.json`.
  - Type a name and press Enter to make a collection of the selected photos, or **Add to
    Collection** (also on the right-click menu) to add them to one.
  - Click a collection to show its photos instead of the folder's; edits, ratings and Export
    work as usual. **Back to Folder** returns. Photos moved or deleted since are left out.
  - **Remove Selected from This Collection** and **Delete This Collection** never touch the files.
- **Stacks** (Lightroom's): **Ctrl+G** (or Photo > Group into Stack) groups the selected photos,
  such as a burst or the frames of a panorama, into one stack shown as its first photo, with the
  count in a badge. **S** expands or collapses it (the others show "2/5" and so on);
  **Ctrl+Shift+G** unstacks. A stack is kept in each photo's sidecar and stays together when
  sorting.
- **Find Duplicates** (the toolbar's **Photo** menu): looks for photos that are the same file
  copied, or the same picture re-saved, resized, re-encoded or turned by 90° or 180° (a 64-bit
  picture hash of a small preview). It runs in the background; the groups then show under the **Duplicates** filter with
  an orange "Group N" badge. **Clear Duplicate Groups** forgets them. Nothing is deleted.
- **Copy Edit / Paste Edit** (Ctrl+Shift+C / Ctrl+Shift+V): copy the open photo's whole node
  tree, select other photos, and paste. Each one gets the tree with its own photo in the Image
  Input, and keeps its own rating and flag. Pasting onto the open photo can be undone.
- **Export Selected...** exports each selected photo with its own edit, using the Export
  window's format, size and **File Naming** template, into the Batch output folder (asked for the
  first time).
- **Sort** (the menu on the Library toolbar, as Lightroom's): **File Name**, **Capture Time**,
  **File Type**, **Rating**, **Pick**, **Edit Time**, **Camera**, **Lens**, **ISO** or **Focal
  Length**, and the **A-Z** button beside it reverses the order. Capture time and the camera
  details come from the EXIF (the file's date when a photo has none). The open photo and the
  selection stay as they are, and the order is remembered.
- **Virtual copies** (Lightroom's): **Ctrl+'**, or right-click a thumbnail > **Create Virtual
  Copy**, adds another edit of the same photo, starting as a copy of the current one. It is
  listed after the photo as "(Copy 1)", with its own edit, rating and flag in
  `IMG_1234.CR3.copy1.nlproj`, so you can try a black-and-white and a colour version side by
  side. Right-click > **Remove Virtual Copy...** moves its sidecar to the Recycle Bin (the photo
  and its other edits stay).
- **Grid view (G):** the whole folder as a grid of cards over the window, like Lightroom's
  Library grid, to look over an import. **Grid** on the Library toolbar or View > Library Grid
  opens it too.
  - Click a card to select it, Ctrl+click to add or remove, Shift+click for a range, Ctrl+A for
    all; the arrow keys move (Shift extends).
  - Click the stars under a card to rate it (click the same star again to clear); 0-5, P, X and U
    work on the selection as in the filmstrip.
  - **Double-click** a card (or press Enter or E) to open the photo and go back to the editor;
    **Escape** or **Loupe** goes back without opening one.
  - **Size** (or Ctrl+wheel) changes the card size. The filter, Copy / Paste Edit and Export
    Selected work as in the filmstrip.
- **Compare view (C):** two photos side by side, as Lightroom's Compare: the **Select** (the first
  selected photo) and the **Candidate** (the second selected one, or the next photo).
  - ← / → change the Candidate; **↑** (or **Make Select**) makes it the Select and brings the
    next photo in as the Candidate; **↓** (or **Swap**) swaps them.
  - The wheel zooms both photos together and dragging pans them, to compare focus; **Fit** goes
    back. Click a photo to make it active (white border): 0-5, P, X, U and 6-9 rate it.
  - Double-click, Enter or E opens the active photo; Escape or G goes back to the grid.
- **Survey view (N):** the selected photos tiled as large as they fit, as Lightroom's Survey, to
  narrow a selection down. Click one (or ← / →) to make it active and rate it; the **x** on a
  photo (or **/**) takes it out of the selection. Double-click, Enter or E opens one; Escape or G
  goes back to the grid.
- The views share the buttons **Grid**, **Compare**, **Survey** and **Loupe** (back to the editor),
  and View > Library Grid / Compare / Survey.
- **Thumbnails** load in the background: a RAW's embedded preview, or the photo itself. An edited
  photo (a blue corner) shows its edit, rendered once and kept in the sidecar.

## Recipes

### Colour-selective Saturation

Keep only red things colourful:

1. Image Input into **Split RGB**.
2. The image also into **Saturation > Image**.
3. Split RGB **R** into Saturation **Amount**.

For a cleaner selection, use a **Color Key** (Hue 0, Hue Range 25) into Amount instead.

### False-colour Infrared (Aerochrome style)

Kodak Aerochrome film recorded infrared as red, red as green, and green as blue. Healthy foliage
reflects a lot of infrared, so it turns vivid red or magenta. A normal photo has no infrared data,
so we estimate it: foliage is green, and green foliage is bright in infrared.

1. Image Input into **Split RGB**.
2. An **Expression** on the image, `clamp(g * 1.4 - r * 0.3 - b * 0.3 + (r + g + b) / 6, 0, 1)`,
   makes a fake infrared channel that is bright on foliage.
3. **Combine RGB**: R = the infrared expression, G = the original R, B = the original G.
4. Optionally add **Levels** or **Curves** for contrast, and **Hue Correct** to fine-tune reds.

### Infrared Foliage (lilac / white trees, dark sky)

Open `examples/infrared_foliage.nlproj` and replace the Image Input's file. It is the classic
full-spectrum infrared look: glowing white-lilac leaves, a dark maroon sky with pink clouds and
dark ground. It was fitted against a real infrared/colour photo pair.

The whole effect is one **Infrared Foliage** group with sliders:

- **Shade Lift** / **Lit Foliage:** how bright foliage in shadow and in light becomes.
- **Sky Contrast:** darkens blue sky while clouds and sunset light stay put.
- **Trunks:** keeps dark, colourless pixels inside foliage (trunks, branches) out of the glow.
- **Halation:** a red-orange glow around highlights, as on infrared film.
- **Grain:** film grain, strongest in the mid-tones.

Press **Tab** on the group to see how it works:

1. **Estimate materials.** A lightly blurred copy (Blur, Relative) gives stable colours. Colourful
   pixels with a warm-to-green hue are vegetation. Sunset light hides green, which is why warm hues
   count too. Blue pixels, and bright smooth areas (a low local Deviation), are sky.
2. **Grow the masks** with Blur + Expression (`max(in1, in2 * 1.5)`), so foliage glows into its own
   shadows and the sky reaches the edges of trees. **Sky gaps:** pixels near the sky that have the
   local sky colour (a Blend > Difference against the blurred, sky-weighted colour) are sky seen
   through needles. Without this, the gaps glow and trees get a bright halo.
3. **Estimate infrared brightness N.** Vegetation is bright (`0.1 + 0.9 * sqrt(L)`), the sky is dark
   (`-0.12 + 0.62 * L`) and everything else follows luminance. L is Normalize 0 / 99 %, so the
   result doesn't depend on exposure.
4. **Glow:** blend N towards a blurred copy inside foliage, and add a soft bloom above 0.6.
   **Dark neutral** (dark pixels with little colour) takes trunks out of the foliage mask and the
   bloom.
5. **Normalize 1 / 99 %** (auto-exposure), then a **Color Ramp** from near-black brown through mauve
   to white-lilac, plus a redder ramp mixed in by the sky mask.
6. **Halation and grain:** highlights above 0.7 are blurred and screened back in red-orange, and a
   White Noise texture adds grain.

Combine RGB packs three masks into one image wherever an Expression needs more than two inputs.

### Black-and-white Infrared (720 nm style)

1. Use the fake infrared expression from above, with more foliage weight (for example
   `g * 1.8 - r * 0.4 - b * 0.6`).
2. Mix it with luminance, then **Levels** to crush the sky: blue skies go nearly black in infrared.
3. Add a **Glare > Fog Glow** at low strength for the dreamy infrared halation.

### Golden Infrared (590 nm style)

1. Make the infrared channel as above.
2. Use **Combine HSV** to put a golden hue (about 0.1) with saturation driven by the infrared
   channel over the original brightness.
3. Alternatively, use **Color Ramp** from luminance with stops going blue, then cream, then gold.

### Ultraviolet Look

UV photography shows flowers with dark nectar guides, bright skies, and a violet, low-contrast
look.

1. **Split RGB**, then build the image mostly from the blue channel:
   Combine RGB with R = B × 0.8, G = B × 0.5 and B = B.
2. **Curves** to flatten the midtones and brighten highlights.
3. **Hue Shift** or **Color Balance** towards violet.

### Local Adjustments (Lightroom Masks)

1. Hover the Result and press Shift+M (or click **Add Mask**) and pick a mask. This inserts a
   Basic labelled "Mask 1" before the Output, driven by the new mask.
2. Shape the mask on the Result (drag the handles, or paint), and press O to see it tinted red.
3. Set the look for that area with the Basic's sliders, shown below the mask in the Inspector.
4. To refine a mask, chain masks: wire a gradient into a **Range Mask**'s Mask input to keep
   only the bright sky inside it, or into a Brush Mask to paint areas in or out. Each Add Mask
   stacks another adjustment after the last.
5. **Subject**, **Sky** and **Background** masks are found by AI models (**Select Subject**,
   **Select Sky**). The first one asks you to download its model in the Inspector.

The same graph can be built by hand: any colour node with a mask wired into its Factor.

**Masking any nodes:** to limit the edit of nodes that have no Factor (Sharpen, Blur, a whole
chain), select them, right-click one and pick **Mask Selected Nodes** and a mask. A **Mix**
labelled "Mask N" goes after them, with the image entering the chain in A, their result in B
and the new mask in Factor: the edit shows where the mask is white and the original elsewhere.
This is the Blender compositor way of splitting a matte area off, editing it and recombining.

### Thermal Camera

**Luminance** into a **Color Ramp** with stops going black, then purple, red, orange, yellow and
white.

### VHS / Analog Video

1. **Split YUV**.
2. **Blur** U and V with a large Size X and a small Size Y (colour bleeds sideways).
3. Shift U slightly with **Transform** (X: a few pixels).
4. **Combine YUV**, then add **White Noise** with Blend > Overlay at a low Factor, and an
   **Expression** scanline (`sin(y) * 0.05 + 0.95`) into Blend > Multiply.

### Chromatic Aberration / RGB Split Glitch

**Split RGB**, then **Transform** the R channel a few pixels left and the B channel right (convert
each to an image first by connecting it to the Transform), then **Combine RGB**. Or simply use
**Lens Distortion** with Dispersion.

### Heat Haze / Liquid Warp

**Noise Texture** Fac into both **Displace** X and Y, with Strength 5..20 pixels.

### Green Screen

**Chroma Key**, then **Color Spill** (Green) on the keyed image, then **Alpha Over** onto a new
background. Use **Dilate / Erode** (Feather, -1..-3) on the matte to soften edges.

## Glossary

- **Channel:** a single grayscale value per pixel.
- **Matte / mask:** a channel used to select part of an image (1 = selected).
- **Premultiplied:** colour already multiplied by alpha. Keyer outputs are premultiplied.
- **Proxy:** the reduced-size copy of the image used for fast previews.
- **Linear / sRGB:** light-proportional values versus display-encoded values.
- **Stop:** a doubling (or halving) of light.
