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
- **Inspector** (below the graph): every setting of the selected node, with larger editors for
  curves and ramps.
- **Result** (right): what the Output node receives, or the node you are previewing.

Every panel is a tab that can be dragged. Drop it on the edge of another panel to dock it there,
or outside the window to float it. **View > Reset Layout** puts everything back.
**View > New Viewer** opens an extra image panel that can show any node (see Viewing Intermediate
Results below).

### A First Graph

1. **File > Import Image** (Ctrl+I) or drag an image file onto the window. This creates an Image
   Input node.
2. Right-click in the graph, type `sat`, and press Enter to add a **Saturation** node.
3. Drag from the Image Input's **Image** output onto the Saturation's **Image** input.
4. Drag from the Saturation's output onto the **Output** node.
5. Drag the **Amount** slider on the node. The Result panel updates live.
6. **File > Export Result** (Ctrl+E) writes the result at full resolution.
7. **File > Save** (Ctrl+S) writes a `.nlproj` project file. It stores the graph and the image
   paths (relative to the project), not the pixels, so keep images next to the project.

### Previewing

- **Ctrl+click** a node to show its output in the Result panel instead of the Output node.
  Ctrl+click it again (or use **View > Clear Node Preview**) to go back.
- **Ctrl+Shift+click** cycles through the outputs of a node that has several (for example the
  R, G and B outputs of Split RGB).
- The preview works on a copy of the image scaled to at most 1280 pixels on the long edge, so it
  stays fast. Export always renders at full resolution. Pixel sizes (blur radius, offsets) are
  measured in full-resolution pixels, so the preview matches the export.

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

## Colour Spaces

An RGB pixel can be described in several other ways. Each **Split** node takes an image apart into
the components of one colour space, and the matching **Combine** node builds an image back from
them. Between the two you can edit, swap or rewire components. For example, blur only the colour
while keeping detail sharp, or drive one component from another.

All components are scaled to fit comfortably on channels. The ranges below are the ones NodeLab
uses, which are not always the textbook units.

### RGB and sRGB

- **What it is:** red, green and blue light, each 0..1. This is how images are stored and shown.
- **NodeLab works in sRGB:** the values in your image file, with the display's gamma curve built
  in. 0.5 looks like a middle gray, even though it is only about 21% of the light of white.
- **Linear light:** physically proportional to the amount of light. Adding light, blurring and
  glows are more realistic in linear. Use **Convert Colorspace** to go sRGB > Linear before such
  operations and Linear > sRGB after.
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
- Loads an image file (PNG, JPEG, BMP or TGA; 8 or 16 bits per channel).
- Output: Image.
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

These adjust colour and tone. Their outputs are clamped to 0..1.

**Brightness / Contrast**
- Brightness adds or removes light (-1..1).
- Contrast pushes values away from (positive) or towards (negative) middle gray.

**Saturation**
- Amount 0 = grayscale, 1 = unchanged, above 1 = more colourful (up to 4).
- Connect a channel to Amount to vary saturation per pixel.

**Hue Shift**
- Rotates every colour around the colour wheel by Degrees (-180..180). 120° turns red into green,
  green into blue, and blue into red.

**Exposure**
- Brightens or darkens in photographic stops: +1 doubles the light, -1 halves it.
- Works in linear light, so highlights behave like a camera, not a simple brightness slider.

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

**Split RGB / Combine RGB**
- Split takes an image apart into R, G, B and A channels. Combine builds an image from four
  channels (unconnected ones use their slider).
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
- Outputs a Mask (1 = selected) and the Image with everything else transparent. Invert flips it.
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

**Directional Blur**
- Motion-style blur with several optional parts:
  - **Distance and Angle:** a straight streak.
  - **Spin:** rotates around the centre, like a spinning camera.
  - **Zoom:** zooms from the centre, like a zoom-burst photo.
  - **Center X/Y:** the centre for Spin and Zoom, 0..1 across the image.

**Bilateral Blur**
- Smooths areas while keeping edges sharp (skin smoothing, noise reduction, painterly looks).
- Radius: how far to smooth. Color Sigma: how different colours may be and still be blended.
  Smaller values keep more edges.
- Determinator: an optional image whose edges are used instead of the input's own.

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

### Transform

These move pixels around. Pixels pulled from outside the image are transparent, unless noted
otherwise.

**Transform**
- Moves (X/Y in pixels), rotates (Angle) and scales (Scale) the image around its centre.
- Wrap tiles the image instead of leaving transparent edges; it is good for seamless offsets and
  glitch shifts.

**Flip**
- Mirrors horizontally, vertically or both.

**Crop**
- Keeps the region between Left/Right and Top/Bottom (0..1 fractions of the image).
- Resize Image on: the output is only the cropped region. Off: the image keeps its size and the
  area outside the crop becomes transparent.

**Lens Distortion**
- Distortion: positive for barrel (bulging), negative for pincushion.
- Dispersion: shifts red and blue apart towards the edges for chromatic aberration, a great
  vintage or glitch detail.
- Fit scales the image so no empty corners show.

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
- **Uses:** film grain (blend with Overlay or Add at low Factor), dithering, and digital static.

### Utility

**Reroute**
- A small dot that passes a wire through, to tidy up the layout. Shift+right-drag across a wire
  adds one.

**Switch**
- Passes On or Off through, depending on the On checkbox. Good for A/B testing two branches.

**Split (Compare)**
- Shows A on one side of a line and B on the other: before/after comparisons.
- Position moves the line; Orientation is a Vertical or Horizontal line; Show Line draws it.

**Image Info**
- Outputs the image's Width, Height and Aspect ratio as Numbers.

**File Output**
- Saves whatever is connected to a file (PNG or JPEG) at full resolution when you choose File >
  Export or File > Write File Outputs, or when rendering from the command line.
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
- **Ctrl+Alt+G** ungroups. Groups can be nested.

## Working in the Node Editor

| Action | How |
|---|---|
| Add a node | Right-click empty space, or press Shift+A. Type to search, then press Enter or click. |
| Connect | Drag from an output pin to an input pin (or the other way round). |
| Add a connected node | Drag a wire into empty space and pick a node from the menu. |
| Insert into a wire | Drag a node onto a wire; it is connected when you release. |
| Pull out of a chain | Alt+drag the node. |
| Disconnect | Drag a wire off an input pin and drop it on empty space, or Ctrl+right-drag across wires (knife). |
| Pan / zoom | Drag empty space or middle-drag; mouse wheel zooms. |
| Frame all / selected | Home / . (period) |
| Select | Click; Shift+click adds; Shift+drag box-selects; Ctrl+A selects all. |
| Select linked | L (upstream) / Shift+L (downstream) |
| Move | Drag, or G then move the mouse and click. |
| Duplicate | Ctrl+D, or Shift+D to duplicate and move. |
| Copy / paste | Ctrl+C / Ctrl+V (works between projects) |
| Delete | Delete or X reconnects the wires around the node; Alt+Delete doesn't. |
| Mute | M: the node passes its input straight through (shown with a red line). |
| Collapse | H: hides the node's settings. |
| Rename | F2, or right-click > Rename. |
| Make links | F: connects the selected nodes in a chain. |
| Reroute | Shift+right-drag across wires. |
| Preview | Ctrl+click a node; Ctrl+Shift+click for its next output. |
| Group / ungroup / enter | Ctrl+G / Ctrl+Alt+G / Tab |
| Frame | Ctrl+J frames the selection. Drag the title to move it with its nodes and the corner to resize. Double-click the title to rename it; right-click it for colour and options. |
| Move nodes between frames | Right-click a node > Move to Frame, or right-click a frame title > Move Selected Nodes Here. Alt+P removes nodes from their frame. |
| Undo / redo | Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) |
| Guide | F1 opens this guide at the selected node's entry. |
| View an intermediate node | Right-click it > Open in New Viewer. |
| Pick a colour | Pick next to a colour setting, then click or drag on an image. |

In the image panels, drag to pan and scroll to zoom. The Original and Result panels move together.
Double-click to reset the view.

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
