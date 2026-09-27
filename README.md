# Paintify for OBS

Paintify for OBS is a Windows OBS effect filter that paints a camera, media,
screen capture, or scene with GPU-generated curved brush strokes. Add **Paintify
GPU** under a source's **Effect Filters**. Each filter instance owns a hidden
renderer and a pair of uniquely named Spout textures; the original source passes
through until the first painted frame arrives. No separate Spout OBS plugin is
needed.

The filter exposes look, paint rate, detail threshold, curvature, brush texture,
impasto, relaxation, temporal repaint threshold and optical flow. It uses the
same painter as [paintify-touchdesigner](https://github.com/BrownBearr/paintify-touchdesigner)
and style values aligned with [paintify-blender](https://github.com/BrownBearr/paintify-blender).

## Install on Windows

1. Download `paintify-obs-windows.zip` from the [latest release](https://github.com/BrownBearr/paintify-obs/releases/latest).
2. Close OBS Studio. Extract the zip's `paintify-obs` folder into
   `%PROGRAMDATA%\obs-studio\plugins\`. Windows may ask for administrator
   permission. The plugin DLL should end up at
   `%PROGRAMDATA%\obs-studio\plugins\paintify-obs\bin\64bit\paintify-obs.dll`.
3. Open OBS, right-click a video source, choose **Filters**, and click **+**
   under **Effect Filters** to add **Paintify GPU**. The bundled renderer starts
   automatically; no build tools, FFmpeg, or separate Spout plugin are needed.

The release is for **64-bit Windows, OBS Studio 31.1.x, Direct3D 11, and a GPU
supporting OpenGL 4.6**. On computers with multiple GPUs, OBS and the renderer
need to use the same adapter for Spout texture sharing. The painted output is
opaque because the GPU painter creates an underpainted canvas. The filter shows
the original source while waiting for its first painted frame or if the
renderer is unavailable.

If the filter is missing, check the DLL path above and restart OBS. The newest
file in `%APPDATA%\obs-studio\logs\` should list `paintify-obs.dll` under
**Loaded Modules**. If the filter appears but shows the original source,
check that your GPU supports OpenGL 4.6 and that OBS and Paintify are using
the same GPU.

## Status

The filter was built and exercised in a portable OBS Studio 31.1.2 session on
Windows with the Direct3D 11 renderer: OBS loaded the plugin, started its
bundled painter, and recorded a painted image source. The standalone renderer
also passed a packaged, headless shader-loading smoke test. The author has
confirmed the installed filter works in OBS. Multiple simultaneous filters
and other OBS versions still need testing.

## Build from source

1. Install Visual Studio 2022 Build Tools, CMake, and vcpkg.
2. Get OBS Studio 31.1.x source headers matching the installed OBS 31.1.x.
3. Run `tools/build-live.bat` to build the GPU renderer.
4. Run `tools/build-obs.ps1 -ObsSourceDir C:\path\to\obs-studio` to build the filter.
5. Run `tools/package-obs.ps1` to create `release/paintify-obs-windows.zip`.
   The zip contains the standard `bin/64bit` and `data` layout, install note,
   and license notices for bundled runtime libraries.

The OBS build script creates an import library from the installed `obs.dll` and
uses matching source headers. It does not alter the OBS installation. OBS 31.1.x
and source 31.1.x should match; other major/minor versions require rebuilding
against their headers and checking the filter API.

The OBS plugin source in `obs/` is licensed under GPL-2.0-or-later; see
[`obs/COPYING`](obs/COPYING). The packaged Spout2 and GLFW runtime libraries
retain their own notices in `licenses/` inside the zip.

## Use

Add Paintify GPU to a source's Effect Filters. The bundled renderer path should
be filled automatically. Choose a look and set Painted frames per second to the
stop-motion cadence you want (12 is the default). OBS presents the last painted
texture between updates. Switching among the four shared Blender looks also
loads that look's threshold, curvature, and brush texture starting values;
you can tune them afterward. Enable temporal repaint to carry paint between video
frames; optical flow helps during camera movement but can soften carried paint.
Changing a setting restarts that filter instance's renderer and resets its
temporal state. Multiple filter instances are isolated by Spout sender names.

The renderer below also works standalone for image, batch, and video processing.

A GPU port of [PainterlyImageCreatorWeb](https://github.com/BrownBearr/PainterlyImageCreatorWeb)'s
Hertzmann renderer. The goal is the web version's output, not a new look: the
placement rule, the stroke path, the brush tiles and the impasto lighting are
ports of specific functions in `worker.js` and `brush-texture.js`, calibrated on
the same scales, so a setting tuned in the web UI means the same thing here.

Feature extraction, seed placement, stroke tracing and rasterisation all run on
the GPU. Stroke data never round-trips through host memory — the draw command
itself is written by a compute shader.

On top of the greedy 1998 algorithm it implements **Hertzmann 2001, "Paint By
Relaxation"**, and it paints **video and image batches** directly.

Measured on an RTX 3060 Ti, `impressionist` preset:

| Image | Drawn strokes | gpu-sbr | + relaxation (4 iters) | worker.js (1 CPU thread) |
|---|---|---|---|---|
| 960 x 1280 | 32K | **2.4 ms** | 43 ms | 173,000 ms |
| 1920 x 1080 | 31K | **3.7 ms** | 22 ms | — |
| 2880 x 3840 | 245K | **10.9 ms** | — | — |

The 960x1280 row is the same image and the same preset in both renderers.

Video, end to end including ffmpeg decode and x264 encode:

| Clip | GPU | wall |
|---|---|---|
| 640 x 854, 24 fps | 2.3 ms/frame | 39 fps |
| 1920 x 1080, 24 fps | 3.7 ms/frame | 16 fps |
| 1920 x 1080, + relaxation | 22 ms/frame | 13 fps |

Wall time is dominated by ffmpeg rather than the renderer; `--vpreset ultrafast`
trades file size for throughput.

Output agreement is verified rather than asserted: on that image gpu-sbr differs
from `worker.js` by exactly as much as `worker.js` differs from *itself* across
two runs (it uses `Math.random()`), which is the floor any port can reach.
Per-layer stroke counts agree within 1%. See [docs/comparison.md](docs/comparison.md)
for the method and the numbers.

## Build

Requires the MSVC Build Tools (or any C++17 compiler), CMake >= 3.21, and vcpkg.
Dependencies (`glfw3`, `glad`, `glm`, `stb`, `imgui`, and on Windows `spout2`)
come from `vcpkg.json`.

```sh
tools\build.bat            # loads vcvars64, configures with Ninja, builds
```

Or by hand, from a developer shell:

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

Shaders are read beside the executable when a `shaders` folder is present,
and otherwise from the source tree (`SBR_ROOT_DIR`). **F5** reloads them
without rebuilding.

## Run

**Easiest: double-click `run.bat`.** The window has native file pickers —
`Open image…`, `Open video…`, `Add images…`, `Choose folder…` — and you can
drag files or a whole folder onto it. Pick your input, confirm the output file
or folder, tune with the sliders against a live preview, then hit **Export**
with a progress bar and a Cancel button.

Dropping a video loads one frame as the preview, so every slider acts on real
footage before you commit to encoding the whole clip. There is a scrub slider
to move to a different frame.

From a terminal:

```sh
build\gpu-sbr.exe                                          # interactive, synthetic subject
build\gpu-sbr.exe --in photo.jpg                           # interactive, your image
build\gpu-sbr.exe --headless --in photo.jpg --out painted.png
build\gpu-sbr.exe --in photo.jpg --preset expressionist --impasto 0.3 --impasto-light 0.3

# painterly video; audio is carried over from the input
build\gpu-sbr.exe --video clip.mp4 --out painted.mp4 --temporal-diff 12

# every image in a folder
build\gpu-sbr.exe --batch photos --outdir painted --format jpg

# Hertzmann 2001 relaxation on top of the greedy result
build\gpu-sbr.exe --headless --in photo.jpg --out painted.png --relax 4
```

`--help` lists every flag. The ones worth knowing:

| Flag | Effect |
|---|---|
| `--preset <name>` | `impressionist`, `expressionist`, `pointillist`, `wash` — the web UI's Hertzmann presets, value for value — plus `detail`, which is this project's own |
| `--radii "8,4,2"` | brush radii, coarse to fine, up to 8 layers |
| `--threshold <f>` | Lab error a cell must exceed to earn a stroke (the web's Threshold slider) |
| `--brush-texture <0..1>` | bristle texture strength |
| `--impasto <f>` / `--impasto-light <f>` | paint relief and its lighting; the web's options are 0, 0.15, 0.3, 0.5 |
| `--underpaint blur\|none\|average` | starting canvas |
| `--tensor-sigma <f>` | > 0 swaps the plain Sobel field for the structure tensor |
| `--etf <n>` | Edge Tangent Flow iterations on the flow field (Kang 2007); 0 = off |
| `--etf-radius <f>` | ETF neighbourhood radius in px, default 5 |
| `--flow <n>` | Lucas-Kanade pyramid levels for temporal advection; 0 = off |
| `--params <file>` / `--save-params <file>` | load / write a tuned look |
| `--passes <n>` | painting passes per layer, default 8 (see below) |
| `--relax <n>` | relaxation iterations; 0 = plain Hertzmann 1998 |
| `--relax-area <f>` | `w_area`: how much a stroke must earn its coverage |
| `--relax-remove <f>` | > 0 deletes strokes that do not pay for their area |
| `--video <in>` | paint a video; pair with `--out clip.mp4` |
| `--batch <dir>` | paint every image in a directory, into `--outdir` |
| `--temporal-diff <f>` | how much a cell's source must change to be repainted |
| `--debug-cells` | print each layer's error distribution and threshold pass rate |

Interactive: **Tab** cycle view - **B** hold to see the source - **wheel** zoom -
**drag** pan - **F** fit - **1** for 1:1 - **F5** reload shaders - **S** save PNG -
**drag-drop** an image - **Esc** quit. The left column is the control panel,
with per-stage GPU time from `GL_TIME_ELAPSED` queries above the sliders. The
**view** selector shows `split`, `painted`, or `source`; `split` is a wipe with
a draggable divider, so both halves share one framing and a feature sits in the
same place on either side of it.

**Save params** / **Load params** write the whole tuned look to a small text
file, which `--params` then accepts — so a look found by dragging sliders can
drive a `--video` or `--batch` render without being retyped as thirty flags.

## Video

```sh
build\gpu-sbr.exe --video clip.mp4 --out painted.mp4 --temporal-diff 12
```

Frames stream in and out through ffmpeg as raw RGBA, so nothing hits the disk in
between. Audio is copied from the input when it has any (`--no-audio` drops it).
`--fps`, `--crf`, `--vcodec` and `--vpreset` control the encode.

ffmpeg and ffprobe must be on PATH; `--video` says so plainly if they are not.
They are a child process rather than a linked library on purpose: libav* would
add a large build dependency and a whole container/codec matrix to maintain, and
raw RGBA over a pipe is already the format `setSource()` and `readCanvas()` use,
so there is no conversion on this side at all.

`--frames <dir> --outdir <dir>` still works for pre-extracted image sequences.

### Temporal stability, and what it does not fix

Worth understanding before painting a long clip. Two mechanisms keep strokes
from boiling:

- **Stroke identity is hashed from position, not from the frame counter.** A
  stroke's colour jitter, brush tile and angle jitter depend on where it is, so
  identical content paints identically. `worker.js` uses `Math.random()` here,
  which re-rolls every stroke every frame; `--jitter-per-frame` restores that
  if you want the boiling as an effect.
- **`--temporal-diff <f>`** carries the previous frame's paint forward and
  repaints only the cells whose source moved by more than `f` (0..255). 12 is a
  reasonable start.

Measured frame-to-frame change in the output, on a near-static clip:

| | output change per frame |
|---|---|
| source | 0.002 |
| `--temporal-diff 12` | 0.001 |
| repainting every frame, position-hashed | 4.33 |
| repainting every frame, `--jitter-per-frame` | 12.49 |

So on still or nearly-still footage the result is rock solid, and
position-hashing alone cuts boiling by 65% when every frame is repainted.

**On camera motion, `--temporal-diff` alone does nothing.** Across a pan every
pixel changes, so every cell clears the threshold and the painting is rebuilt
from scratch each frame: strokes stay anchored to the pixel grid while the
content slides past them, which is the classic "shower door" look.

`--flow <levels>` fixes it, by warping the carried canvas along a pyramidal
Lucas-Kanade flow field before the threshold test — so the test asks whether the
*subject* changed rather than whether the pixel did. Measured on a synthetic
6 px/frame pan with exactly known displacement, motion-compensated change:

| `--temporal-diff` | `--flow 0` | `--flow 4` |
|---|---|---|
| 12 | 14.30 | 12.61 |
| 24 | 14.44 | 8.27 |
| 48 | 14.46 | **3.04** |
| 250 (nothing repaints) | 16.49 | **0.82** |

The left column is the point: without flow, raising the threshold never helps
and eventually makes things worse, because the paint freezes in place while the
content moves out from under it. There is no setting at which it is stable.
With flow, the knob works — 4.8x better at matched settings.

Two caveats, both measured rather than assumed. What is advected is the
**canvas, not a stroke list**, so no stroke is re-oriented or moved as an
object. And advecting an image means resampling it every frame it survives:
Catmull-Rom sampling and an exact-copy fast path make a still camera *exactly*
lossless, but sustained panning still softens the painting by roughly half over
ten frames at a high repaint threshold. [docs/flow.md](docs/flow.md) has the
tables and the reasoning.

## Batch images

```sh
build\gpu-sbr.exe --batch photos --outdir painted --format jpg --suffix _paint
```

Each image is independent — no temporal carry-over — and the size may change
between files. Unreadable files are skipped with a message and counted rather
than treated as fatal. Output names are `<stem><suffix>.<format>`.

## Pipeline

Per layer, coarse radius to fine:

| Stage | File | Ported from | What it does |
|---|---|---|---|
| 1 | `blur.comp` | `gaussianBlurRGB`, `makeGaussKernel` | Separable Gaussian at sigma = radius/2 — the layer's reference image |
| 2 | `features.comp` | `computeGradients`, `computeGradientsST` | Sobel on the reference, or the smoothed structure tensor |
| 3 | `error.comp` | `computeErrorMap`, `chooseBestInCell` | Lab error between reference and canvas, reduced per seed cell to (mean, argmax) |
| 4 | `seeds.comp` | the `cells` loop in `paintHertzmann` | A cell earns a stroke only where mean error exceeds the threshold; colour and jitter from the reference at the argmax pixel |
| 5 | `trace.comp` | `makeCurvedStroke` | Walks the gradient field with Hertzmann's fc filter, stopping when the canvas already matches the reference better than this stroke would. Also writes the `DrawArraysIndirect` command |
| 6 | `stroke.vert/.frag` | `renderStrokeSolid`, `drawThickSegmentTextured` | One triangle-strip instance per stroke; coverage from the polyline distance, modulated by the brush tile |
| 7 | `impasto.comp` | `applyImpastoLighting` | Lights the accumulated height field (Hertzmann 2002) |

Then, only when `--relax` is on:

| Stage | File | What it does |
|---|---|---|
| 8 | `pool.comp` | Collects every stroke of the frame into one pool, so relaxation can revisit them |
| 9 | `relax.comp` | Hertzmann 2001: perturbs each stroke's control points, width and colour against the energy |
| 10 | `energy.comp` | Reduces the global appearance energy, so relaxation can be checked rather than believed |

And the two stages with no web counterpart, each only when asked for:

| Stage | File | What it does |
|---|---|---|
| — | `etf.comp` | `--etf`: edge-aware smoothing of the layer's direction field (Kang 2007), plus the coherence reduction that verifies it |
| — | `flow.comp` | `--flow`: pyramidal Lucas-Kanade between consecutive source frames, for temporal advection |

Brush tiles come from `src/brush_atlas.cpp`, a port of `makeBrushTile` including
its `mulberry32` PRNG, so tile *n* for a given radius is the same content the
web version generates.

### Passes per layer

`worker.js` paints one stroke at a time, and `makeCurvedStroke` stops a stroke
once the canvas already matches the reference better than the stroke's own
colour would. Every stroke therefore sees the paint of every earlier stroke in
its own layer, and that feedback is what sets stroke length. Painting a whole
layer in one parallel pass removes it, and strokes come out too short.

So each layer is painted in several passes over disjoint, hash-scattered subsets
of its cells: a stroke in pass *k* sees the paint from passes 0..*k*-1. Eight
passes reproduces the web version's mean stroke length to within 1%; `--passes`
exposes the knob. Hash-scattering rather than splitting by index matters,
because contiguous subsets would each cover a band and show their seams —
`worker.js` shuffles its cell list to decorrelate overlap order from position
for the same reason.

The count is also floored by capacity: 2880x3840 at radius 2 is 2.76M cells
against a 200K stroke buffer. Either way each cell is evaluated exactly once and
the error map is still built once per layer, so passes never change *which*
cells earn strokes.

### Other deliberate deviations

- **One instance per stroke, not per segment.** The web version unions a
  stroke's segments into a single mask and composites it once. Drawing each
  segment as its own blended quad is *not* equivalent — joins and overlaps on a
  curve composite twice, which reads as translucent wire instead of one opaque
  mark. A triangle-strip instance with miter joins covers each fragment once,
  which reproduces the single-composite behaviour.

- **Continuous dry brush.** `renderStrokeSolid` fades opacity per segment when
  dry brush is on. The ribbon has no per-segment compositing to fade, so the
  same ramp is applied continuously over arc length — the limit of that loop as
  segments shrink.

- **Temporal coherence is the web's rule by default.** A cell keeps last
  frame's paint unless its source moved by more than `--temporal-diff`.
  `--flow` adds Lucas-Kanade advection on top of that rule rather than
  replacing it, and is off unless asked for.

- **Not ported:** palette quantisation (`buildPalette` / k-means), the salience
  and manual detail maps, and the Litwinowicz / Haeberli / pencil / neural
  algorithms. Only `paintHertzmann` is here.

### Beyond the port: ETF and flow

Two stages have no counterpart in the web renderer. Both are **off by default**,
because the port's verification method is comparison against `worker.js` and a
stage `worker.js` does not have cannot take part in it. With `--etf 0 --flow 0`
the output is the web port's.

Neither can be checked against a reference implementation, so each ships with
its own measurement instead — a picture that merely looks different is not
evidence that a filter works.

**`--etf <n>` — Edge Tangent Flow** (Kang, Lee & Chui 2007). An edge-aware
smoothing of the per-layer direction field: weak pixels defer to strong
neighbours, nothing is smoothed towards a perpendicular flow, and a tangent and
its negation count as one direction. A Gaussian expresses none of those, which
is why `--tensor-sigma` does not solve the same problem.

`--etf-log` reports flow coherence — the mean alignment of each tangent with its
eight neighbours, where 0.64 is what a random field scores. It rises
monotonically with iterations in every layer, and most where the plain Sobel
field is noisiest:

| iterations | r 8 | r 4 | r 2 |
|---|---|---|---|
| 0 | 0.961 | 0.903 | 0.830 |
| 2 | 0.978 | 0.952 | 0.912 |
| 3 | 0.984 | 0.970 | **0.948** |

Strokes then run slightly longer and about 7% fewer are placed, concentrated in
the fine layers — a better coarse field leaves less residual error behind it.
Cost is roughly 3 ms per iteration at 960x1280.
[docs/etf.md](docs/etf.md) has the method and the full table.

**`--flow <n>` — optical flow advection.** See *Temporal stability* above.

### Relaxation

`--relax <n>` adds Hertzmann 2001 on top of the greedy result, perturbing the
control points, width and colour of the strokes already placed and keeping
whatever lowers

    E = sum|Lab(canvas) - Lab(source)| + w_area * sum(stroke area)

`--relax-log` prints the energy per iteration. Most of the gain arrives in the
first iteration and almost all of it by the fourth. The area weight is the
interesting control — it decides how much a stroke has to earn its coverage —
and together with `--relax-remove` it reaches a *lower* appearance error than
the greedy result while using roughly a tenth of the paint.

Two things about it are worth reading before tuning: the area term is easy to
formulate degenerately, and "the paint underneath a stroke" must not be measured
from the canvas that already contains it. Both were live bugs here, both looked
fine in the output, and the energy log is what caught them.
[docs/relaxation.md](docs/relaxation.md) has the derivation, the measurements
and the approximations.

### An upstream quirk worth knowing

At `curvature: 0.0` the web's stroke walk is degenerate on its first step and
every stroke comes back with a single point, which draws nothing — so the web's
own `pointillist` preset paints only the underpaint. gpu-sbr seeds the initial
direction from the gradient instead, which is bit-identical for every curvature
above 0 and turns curvature 0 into the straight dabs the preset intends.
Details in [docs/comparison.md](docs/comparison.md).

## Verifying against the web version

See [docs/comparison.md](docs/comparison.md): `worker.js` runs unmodified in
Node under a `vm` context, so both renderers can be given the same image and
compared per layer. That comparison is what caught the two real bugs in this
port, neither of which was visible in the output image.
