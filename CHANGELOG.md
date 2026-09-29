# Changelog

NodeLab uses [semantic versioning](https://semver.org). While it is 0.x, minor versions add
features and patch versions fix bugs. Each release is tagged `vX.Y.Z` in git.

## Unreleased

- Move nodes between frames: node right-click → Move to Frame, frame right-click → Move Selected
  Nodes Here, Alt+P removes from frame.

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
