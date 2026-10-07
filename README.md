# NodeLab

Node-based image manipulation. Import a photo, wire color data between nodes, and see the original
(left) and the result (right) update live. Aimed at channel mixing, IR/UV camera emulation, and
glitch/experimental looks.

## Download

Get `NodeLab.exe` from the [latest release](https://github.com/balthazarschmitt/NodeLab/releases/latest).
It's a single portable exe for 64-bit Windows 10/11 (OpenGL 3.0 or newer) and needs no install.
The zip also has the guide, the changelog and example projects. Help > Guide shows the same guide
inside the app.

The AI masks (Select Subject, Select Sky) download their models on first use, from the
Inspector: about 240 MB for Subject and 190 MB for Sky, into `%APPDATA%\NodeLab\models`.

## Build (Windows)

Requires CMake ≥ 3.24 and a C++20 compiler. CLion's bundled MinGW toolchain works out of the box:
open the folder in CLion, pick the `NodeLab` target, Run. Dependencies (GLFW, Dear ImGui,
nlohmann/json, stb, tinyexpr, LibRaw, zlib-ng, doctest) are downloaded by CMake on first configure.

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
PNG, JPEG, TIFF or OpenEXR; `--depth 32` for full-float EXR; `--timings` prints evaluate and save times)
Headless batch: `NodeLab.exe --batch project.nlproj outDir [--png|--jpg|--tif|--exr] [--depth N] a.jpg b.jpg ...`
(each source goes into the project's first Image Input and is saved as `outDir\<name>_edit.<ext>`)
Benchmark: `NodeLab.exe --benchmark project.nlproj [--full] [--runs N] [--sync]` (median ms per node)
GPU compositing: `--device gpu|cpu` and `--precision half|full` for `--render` and `--benchmark`
(both use the CPU unless given `--device gpu`; `--precision` is full for `--render` and half for
`--benchmark` by default; the app itself uses the GPU, View > Compositor); `NodeLab.exe --gpu-info` names the GPU device or says why
there is none. In the app: View > Compositor.
AI models: `NodeLab.exe --install-model subject|sky` downloads one (and the ONNX Runtime) without
the UI.
Open an image directly (also works with Windows' Open with): `NodeLab.exe photo.CR2` starts a new
project with it.

Screenshot of the UI (debug aid): `NodeLab.exe project.nlproj --screenshot shot.png`
Scripted UI test (feeds input straight to ImGui, ignores the real mouse; see `src/ui/UiScript.h`):
`NodeLab.exe examples\demo.nlproj --script tests\ui\fanout_addnode.txt`

Try `examples/demo.nlproj`: the red channel drives saturation, so only red things stay colorful.
`examples/infrared_foliage.nlproj` turns a colour photo into lilac-white infrared foliage under a
dark sky, as one group with sliders (see Help > Guide > Recipes).

## Using it

| Action | How |
|---|---|
| Add a node | Right-click the canvas (or Shift+A) and start typing to search (Up/Down + Enter), or browse the menus (the wheel steps through a menu's nodes, Enter adds) |
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
| Make links | F connects the selected nodes left to right; then 1 / 2 step the new wire's output / input, F the next pair, Esc keeps it |
| Detach links | Alt+D removes the wires between the selected nodes (wires to other nodes stay) |
| Swap links | Alt+S: two nodes trade places and wires; one node swaps its two wired inputs |
| Mask selected nodes | right-click → Mask Selected Nodes: a Mix after them blends their edit with the original through a new mask |
| Select linked | L upstream, Shift+L downstream |
| Cut wires / add reroutes | Ctrl+right-drag / Shift+right-drag across wires |
| Preview another output | Ctrl+Shift+click a node cycles through its outputs |
| Frame all / selected | Home / . |
| Find a node | Ctrl+F (View > Find Node...): type part of a label or node name, Enter selects and frames it (opening the group it is in) |
| Edit a value | Drag the field sideways (Shift = fine), or click it to type |
| Preview any node | Ctrl+click it (again to clear) |
| Group / ungroup | Ctrl+G / Ctrl+Alt+G; Tab (or double-click) enters a group, Tab leaves |
| Group pins | Select the group (or its Group Input/Output inside) and edit in the Inspector; Channel and Number inputs have a Default / Min / Max and show a slider on the group node. Inside a group, Add > Group > Value Input / Value Output adds a socket as its own node (F2 renames it) |
| Presets | Right-click a node > Save as Preset...; Add > Presets inserts it into any project (files in `%APPDATA%\NodeLab\presets`) |
| Frame | Ctrl+J around the selection; drag its title to move it with its nodes, corner to resize, double-click to rename, right-click for color |
| Move nodes between frames | right-click a node → Move to Frame, or select nodes and right-click a frame title → Move Selected Nodes Here; Alt+P removes from frame |
| Panels | Drag a panel's tab to dock it elsewhere, or out of the window; View > Layout picks a preset (Default, Compositing, Photo, Side by Side, Node Focus); View > Reset Layout |
| Inspector | Floats in the Node Editor's corner while a node is selected; Preferences > Interface (or View > Inspector Overlay) makes it a panel |
| Edit a value | Double-click a slider (or click a node's value box) to type; Backspace over it, or right-click, resets it |
| Reset to defaults | Right-click a node → Reset to Defaults (keeps its file) |
| Preferences | Edit > Preferences: layout, Inspector, themes (NodeLab Dark, Blender, Darkroom, Midnight, High Contrast, Light, or your own colours), viewer background, compositor device, new projects' view transform |
| Extra viewers | Right-click a node → Open in New Viewer (or View > New Viewer) to watch an intermediate result; the viewer's drop-down switches node, "Sync view" pans with the other panes |
| Eyedropper | "Pick" next to a colour setting (or Pick from Image in the node's colour popup), then click a pixel or drag a rectangle on any image panel for the area's average; right-click / Esc cancels |
| Guide | Help > Guide or F1 (opens at the selected node's entry); also the Inspector's Guide button |
| Zoom / pan images | Mouse wheel / drag; double-click resets. Both panes stay in sync and sharpen to full resolution when zoomed in |
| Colour management | Color menu: View Transform (Standard, AgX, Raw), Look, view Exposure and Gamma, as in Blender's Render Properties. New projects are scene-linear; Convert Project to Scene-Linear upgrades a legacy one |
| Histogram / clipping | Result toolbar, or H / J with the mouse over the Result: RGB histogram, and clipped highlights in red and crushed shadows in blue |
| Before / after | Result toolbar, or Y with the mouse over the Result: the original left of a draggable divider, the edit right; `\` shows the original alone |
| Transparency | Images with alpha are drawn over a checkerboard |
| On-image controls | Select a Crop, gradient, shape or Brush Mask node and edit it on the Result: drag handles; Crop shows the whole frame (drag outside to straighten); Brush paints, Alt erases, `[` `]` size; O toggles the red mask overlay, or with Crop cycles its guide overlay (Shift+O turns it); Perspective: drag to draw Guided Upright guides, Alt+click removes |
| Loupe overlay | View > Loupe Overlay: a grid and draggable guide lines over the Result |
| Spot removal | Select a Spot Removal node, click a blemish on the Result (the source is picked automatically, `/` finds another), or drag its source onto clean texture; drag a spot to move it, its edge to resize; Alt+click or Delete removes one |
| Auto tone | Basic's Inspector → Auto sets Exposure, Contrast, Highlights, Shadows, Whites and Blacks from the image |
| Auto save | Every 5 minutes by default (Edit > Preferences > Save & Load), once a project has been saved (library photos always) |
| Add Mask | Result toolbar → Add Mask (Shift+M over the Result), or M linear, Shift+R radial, K brush (also Luminance Range and the AI masks Subject, Sky and Background): inserts a Basic labelled "Mask N" before the Output, driven by a new mask; the Inspector shows the mask with the Basic's sliders below it |
| Import image | File > Import Image, or drop a file on the window |
| Library | File > Open Folder (Ctrl+Shift+O), drop a folder on the window, or `NodeLab.exe <folder>`: a filmstrip of the folder's photos. Click or ←/→ to open one; its edit is saved automatically in `<photo>.nlproj` beside it. New photos start as Image Input → Denoise → Basic → Output |
| Culling | In the Library: 0-5 rate, P pick, X reject, U unflag; Ctrl+click / Shift+click select several; the filter menu shows picks, stars, rejects or edited photos |
| Library sort | The Library toolbar's Sort menu: file name, capture time, file type, rating, pick, edit time, camera, lens, ISO, focal length; A-Z / Z-A |
| Virtual copies | Ctrl+' (or right-click a thumbnail): another edit of the same photo, with its own sidecar, rating and flag |
| Library grid | G (or the Library's Grid button): the whole folder as cards; click the stars to rate, double-click / Enter / E opens a photo, Esc goes back; Ctrl+wheel resizes |
| Copy / paste edit | Ctrl+Shift+C copies the open photo's edit, Ctrl+Shift+V pastes it onto the selected photos (each keeps its own file and rating) |
| Export selected | Library → Export Selected... (or File menu): each selected photo exported with its own edit, using the Export window's format and size |
| Export | File > Export (Ctrl+E) opens the Export window: renders in the background with a progress bar and Cancel. Format (PNG/TIFF 8 or 16 bit, JPEG + quality with EXIF, OpenEXR half/full float scene-linear), size (original, long edge, percent; Lanczos in linear light) |
| Batch | Export window → Batch: add files or a folder (or drop them on the window), pick the Image Input they feed and an output folder; each result is named by the File Naming template (`{name}_edit`, with tokens for sequence, capture date, camera, lens...) |
| Export presets | Export window → Preset: Full-Size JPEG, Web JPEG, Email, PNG, 16-bit TIFF, OpenEXR, or Save... your own |
| Snapshots | View > Snapshots: Create Snapshot keeps the whole edit (saved with the project); click one to restore it |

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
| Filter | Blur (pixels or Relative %), Directional Blur (+spin/zoom), Bilateral Blur (both with a Fast quality), Denoise (Luminance/Color, wavelets), Sharpen (Amount, Radius, Detail, Masking), Spot Removal (Heal / Clone, on the Result), Filter (Soften, Sharpen, Laplace, Sobel, Prewitt, Kirsch, Shadow), Dilate / Erode, Kuwahara, Pixelate, Posterize, Glare (Fog Glow, Streaks, Simple Star), Sun Beams, Grain (Amount, Size, Roughness, as Lightroom) |
| Transform | Transform, Flip, Crop (straighten, aspect presets, on-image frame), Lens Correction (distortion, fringing, vignetting), Lens Profile (lensfun profiles picked from the photo's EXIF), Lens Distortion (with chromatic dispersion), Perspective (Lightroom's Transform: Guided Upright, Vertical/Horizontal), Displace, Map UV, Corner Pin |
| Matte | Box Mask, Ellipse Mask, Radial Gradient, Linear Gradient, Brush Mask (painted on the Result, with Auto Mask), Range Mask (luminance or colour range), HSL Mask (hue, saturation and lightness qualifier), Select Subject (Accurate or Light model) and Select Sky (AI masks, models downloaded on first use), Channel Key, Luminance Key, Difference Key, Distance Key, Chroma Key, Color Spill, Double Edge Mask |
| Texture | Noise, Voronoi, Gradient, Wave, Checker, White Noise |
| Utility | Reroute, Switch, Split (compare), Image Info, File Output (PNG/JPEG/TIFF/OpenEXR; written on single Export / File > Write File Outputs / --render) |
| Group | Groups (Ctrl+G) with Group Input / Group Output inside, and Value Input / Value Output for single sockets |

Sizes in pixels (blur radius, offsets, glare size) refer to the full-resolution image; the preview
scales them so it matches the export. Textures use image-relative coordinates for the same reason.

Color adjustment outputs are clamped to 0..1 and their parameters to their ranges. Math and
converter nodes are unclamped (enable Clamp where offered) so intermediate values can go
negative or above 1.

Expression variables: `r g b a` (Image input), `in1 in2`, `x y` (pixel), `u v` (0..1), `w h`;
functions include `sin cos pow sqrt abs floor ceil log exp atan2 min max clamp mix step smoothstep fract`.

## Layout

```
src/core    Image/Channel/Value types, conversions, colour maths and science, curves, ramps, noise,
            parallelFor
src/graph   Node model, Graph (links, frames, JSON), Evaluator (cache levels, regions, background
            thread), node registry, Add menu layout, recipes (Add Mask)
src/gpu     GPU device (OpenGL 4.3 compute), per-pixel kernels and their fusion (PointOp), GPU blur,
            reductions (exact percentiles), the viewer's display transform
src/nodes   Node implementations, one file per family (io, color, math, converter, filter,
            transform, matte, texture, utility, group)
src/io      Image load/save (PNG, JPEG, TIFF, EXR, RAW via LibRaw), ICC, EXIF, export, source image
            cache, project files, the photo library's sidecars, presets
src/ml      AI models: downloads (WinHTTP, pinned SHA-256), ONNX Runtime loaded at run time
src/ui      App window, node editor (custom canvas), inspector, image views and overlays, library,
            preferences and themes, file dialogs, scripted UI tests
tests       doctest unit tests (tests/golden: hashes that keep legacy renders byte-identical)
```

Adding a node: write a class with a `NODELAB_NODE({...})` descriptor and `evaluate()`, then
`r.add<YourNode>()` in its family's register function.

## License

NodeLab is released under the [MIT License](LICENSE). The release exe statically links GLFW (zlib),
Dear ImGui (MIT), nlohmann/json (MIT), stb (public domain / MIT), tinyexpr (zlib), zlib-ng (zlib)
and LibRaw (LGPL 2.1 / CDDL 1.0, used under CDDL). The AI masks download ONNX Runtime and DirectML
(MIT) and their models (BiRefNet and U²-Net sky segmentation, MIT) on first use; they aren't
part of the exe. `third_party/onnxruntime` has ONNX Runtime's C API headers (MIT).
