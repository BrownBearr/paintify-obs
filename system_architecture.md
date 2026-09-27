# gpu-sbr architecture

## OBS host bridge

The Windows OBS effect filter (`obs/paintify-filter.cpp`) captures its upstream
source to a Direct3D 11 render texture and publishes it through a uniquely
named SpoutDX sender. A separate hidden `gpu-sbr.exe` receives that texture
with SpoutGL, runs the painter on the GPU, and publishes the canvas under a
second unique name. The filter opens the shared output texture and draws it
in OBS, reversing SpoutGL's vertical texture axis. Each filter instance has
its own child process, sender pair, and stop file. If the renderer is missing
or has not published a frame, OBS renders the original source.

Filter settings are copied under a mutex and applied on the OBS graphics
thread. A setting change restarts the child process, resetting temporal paint
state. The filter throttles input transfers to the selected paint rate while
OBS reuses the most recent painted texture between updates. The renderer also
checks the OBS process ID and stop file so it exits after its host goes away.

This renderer exists to reproduce [PainterlyImageCreatorWeb](https://github.com/BrownBearr/PainterlyImageCreatorWeb)'s
`paintHertzmann` output on the GPU. That constraint decides most of the design:
where a choice is between "what maps well to a compute grid" and "what the web
version does", the web version wins unless the two are provably equivalent.

Every function name in parentheses below refers to `worker.js` or
`brush-texture.js` in that repository. If you change a stage, read the function
it is ported from first.

## Why the first version did not work

Worth recording, because the failure was structural rather than a matter of
tuning. The original renderer scattered strokes by blue-noise-thresholding the
*edge magnitude* of the source. That is not Hertzmann's algorithm: Hertzmann
places a stroke where the **canvas differs from the reference**, which is a
feedback loop between the layers. Without it, fine layers pile onto edges
regardless of whether the coarse layer already got the colour right, and flat
regions that are simply the wrong colour are never corrected. Three other
defects compounded it:

- one gradient field for every layer, computed at full resolution, so coarse
  strokes chased pixel-level tangents instead of coarse structure;
- strokes terminated on an edge threshold rather than on the canvas-vs-reference
  test, so they ran to full length nearly always;
- each polyline *segment* was a separately blended quad, so joins and curve
  overlaps composited twice and strokes read as translucent wire.

The result was a tangle of filaments over a sharp copy of the photograph (the
"blur-scale" underpaint blit was `GL_LINEAR` between identical rectangles, i.e.
an exact copy). All four are fixed; the stage table below is the current
pipeline.

## Data flow

```
              src (RGBA8, mipped for the 'average' underpaint)
                │
       ┌────────┴──────── once per layer, coarse radius → fine ───────────┐
       │                                                                  │
   blur.comp ──► ref (RGBA16F)                                            │
       │           │                                                      │
       │      features.comp ──► grad (gx, gy, gmag)                       │
       │           │                                                      │
   error.comp ─────┴──► err (R16F) ──► error.comp pass 1 ──► CellBuf      │
       │                                    (mean error, argmax pixel)    │
       │                                              │                   │
       │        ┌──── once per pass ──────────────────┴─────────────┐     │
       │        │                                                    │     │
       │   seeds.comp ──► SeedBuf ──► trace.comp ──► VertexBuf       │     │
       │                                  │            HeaderBuf     │     │
       │                                  └──► IndirectBuf           │     │
       │                                          │                  │     │
       │                          stroke.vert/.frag (MRT)            │     │
       │                                  │                          │     │
       └──────────────────────────────────┴──► canvas (RGBA16F)  ────┘     │
                                               height (R16F)              │
                                                  │                        │
                                          impasto.comp ──► canvas          │
```

The canvas is both a render target and an input: `error.comp` and `trace.comp`
sample it. The framebuffer is therefore unbound before every compute stage and
rebound for the draw, with `glTextureBarrier()` after each pass' draw.

## Stages

### 1. `blur.comp` — reference image (`gaussianBlurRGB`, `makeGaussKernel`)

Separable Gaussian at `sigma = max(0.1, radius * 0.5)`, radius
`ceil(sigma * 2.5)`, weights normalised over that finite window, clamp-to-edge.
The kernel is evaluated inline rather than uploaded: 31 `exp()` calls is cheaper
than the extra binding and it keeps sigma a plain uniform.

**A mip level is not a substitute.** A box-filtered mip leaves blocky
low-frequency structure that every stroke colour then inherits. The original
renderer sampled `textureLod(src, uv, log2(radius))` here and it is visible in
the output.

### 2. `features.comp` — flow field (`computeGradients`, `computeGradientsST`)

Sobel on the reference blur. Output is `(gx, gy, gmag)`, and the plain path
stores the **raw** Sobel sums deliberately: `makeCurvedStroke` blends
`curv * gradientPerp + (1 - curv) * lastDir` against a unit `lastDir`, so at
curvature < 1 the gradient's magnitude carries real weight in the curve's
shape. Normalising here would silently change it.

`tensorSigma > 0` switches to the structure tensor: raw components → Gaussian
smooth (reusing stage 1) → dominant eigenvector, which is how
`computeGradientsST` reuses `gaussianBlurRGB` on a packed RGB buffer. That path
*does* emit a unit vector, matching the JS.

Recomputed **per layer**. This is what makes a radius-8 stroke follow radius-8
structure.

### 3. `error.comp` — placement test (`computeErrorMap`, `chooseBestInCell`)

Pass 0: per-pixel Lab distance between reference and canvas. The encoding is
OpenCV's uint8 convention (`L*255/100`, `a+128`, `b+128`) because the web UI's
`threshold` slider is calibrated against it — changing the encoding silently
rescales the threshold.

Pass 1: one invocation per seed-grid cell, reducing to `(meanErr, argmax
pixel)`. Ties are broken by a hash-derived `1e-3` nudge so flat cells do not all
seed at their top-left corner, which is what `Math.random() * 1e-3` does in the
JS.

Run **once per layer**, against the canvas as it stands before the layer paints.
`worker.js` does the same — it builds `err` before the cell loop and lets the
loop mutate the canvas underneath it — so this is the faithful choice as well as
the cheap one.

### 4. `seeds.comp` — the cell loop

One invocation per cell. A cell earns a stroke only when
`meanErr > threshold`, except on the coarsest layer, which always paints so the
canvas starts covered. Colour is the reference blur at the argmax pixel, then
HSV jitter (`finalizeStrokeColor`); size and opacity jitter are
`value * (1 + (rand*2-1) * amount)`, the JS's two-sided form.

The brush tile variant is hashed from the **seed position**, via a port of
`brushHash`, never from a frame counter — otherwise a video's strokes change
texture every frame.

### 5. `trace.comp` — stroke path (`makeCurvedStroke`)

One thread per seed, `step = max(1, round(radius))`. Terminates on, in order:
leaving the image; the canvas already matching the reference better than this
stroke's colour would (`dCan <= dStr`, only once `iter >= minLen`); a gradient
magnitude floor; a degenerate direction. It does **not** stop at edges — that is
Litwinowicz's rule, not Hertzmann's.

The termination test re-samples the reference at the seed rather than using the
seed's stored colour, because the stored colour has already been through the HSV
jitter and the JS compares against the unjittered reference.

Thread 0 also writes the `DrawArraysIndirect` command, so stage 6 needs no
readback.

### 6. `stroke.vert` / `stroke.frag` — rasterisation (`renderStrokeSolid`)

**One triangle-strip instance per stroke**, two vertices per polyline point,
with miter joins (clamped, or a hairpin throws a spike across the canvas).
Points past the stroke's length collapse onto the last one, emitting zero-area
triangles.

Per *segment* instancing is not equivalent and was the original renderer's worst
rendering bug. `renderStrokeSolid` unions a stroke's segments into one mask and
composites it **once**; blending each segment separately double-composites every
join and every curve overlap.

Coverage is `clamp(rEff - dist + 0.5, 0, 1)`, matching the JS, with
perpendicular distance taken as `|vNorm| * rEff` — exact on a straight segment,
since the ribbon edges are at `|vNorm| == 1` by construction. The two outermost
points are pushed out along the tangent by a radius so `vArcPx` runs negative
(or past `totalLen`) inside the caps, and that overshoot becomes the cap's
curvature: a proper round cap, which is what `drawCircle` gives the JS mask.

Two render targets: colour blends premultiplied source-over, height accumulates
additively, matching `heightBuf[i] += mv * impastoStrength` — note height takes
the **pre-opacity** coverage.

### 7. `impasto.comp` — relief lighting (`applyImpastoLighting`)

Hertzmann 2002. The detail worth preserving is that lighting is relative to
flat: a pixel with no relief has normal `(0,0,1)` and produces `light == 1`, so
bare canvas keeps its colour instead of the whole image being multiplied down.
`gain = 2.5` and the `[0.35, 1.8]` clamp are the JS's.

The web exposes strength and light as Off/Subtle/Medium/Strong = 0/0.15/0.3/0.5,
and the sliders here stop at 0.5 for the same reason: past roughly 0.6 the height
field saturates in dense areas and the lighting clips to its clamps everywhere,
which loses all colour.

## Passes per layer

Two independent reasons a layer is painted in several passes.

**Capacity.** A layer's seed grid can want more strokes than the stroke buffers
hold — at 2880x3840 with radius 2 and `gridFactor` 1 that is 2.76M cells
against a 200K buffer.

**Intra-layer feedback, which is the important one.** `worker.js` paints one
stroke at a time, and `makeCurvedStroke` stops a stroke once the canvas already
matches the reference better than the stroke's own colour would. Every stroke
therefore sees the paint of every earlier stroke *in its own layer*, and that
feedback is what sets stroke length. Paint a whole layer in one parallel pass
and the feedback does not exist: nothing in the layer has been painted when the
strokes trace. Measured, that is mean 5.44 points per stroke against the web's
7.03. With `passesPerLayer` passes, a stroke in pass *k* sees the paint from
passes 0..*k*-1, and eight passes lands within 1% of the web version — see
`docs/comparison.md` for the sweep.

Cells are assigned to passes **by hash**, not by index. Contiguous subsets would
each cover a horizontal band and show their seams where strokes overlap;
`worker.js` shuffles its cell list to decorrelate overlap order from position,
and hashing achieves the same. Each cell is still evaluated exactly once, and
the error map is still built once per layer, so passes never change *which*
cells earn strokes — only the order in which they are painted.

## Buffer sizes

| Buffer | Size at 200K strokes |
|---|---|
| `SeedBuf` | 6.4 MB (32 B/stroke) |
| `VertexBuf` | 52.8 MB (33 points x 8 B) |
| `HeaderBuf` | 3.2 MB (16 B/stroke) |
| `CellBuf` | grows to the largest grid; 33 MB at 1 cell/px on 8 MP |

Vertices are 8 bytes — `packHalf2x16(pos)` and `packHalf2x16(radius, arc)`.
Colour is per-stroke and lives in `StrokeHeader`, not on every vertex.

## Telemetry

Each stage has a double-buffered `GL_TIME_ELAPSED` query that is read one frame
late, so timings never stall. Stages 4-6 run once per (layer, pass), so the
reported figure sums `lastMs` after every `end()` — the frame total shifted by
one measurement, which settles immediately.

Stroke counts are copied GPU-side into a stats buffer and read at the top of the
next frame. Reading the counter inside the pass loop pulls a buffer from video
to host memory mid-frame and serialises the pipeline; it cost about 4x on a
960x1280 frame.

## Orientation, and a bug worth not repeating

Every compute stage addresses its textures **by texel**, with row 0 = image row
0. The rasteriser must agree, so `stroke.vert` maps image y directly to NDC y
and does **not** negate it: window y 0 is the framebuffer's bottom row, which is
texel row 0 of the attached canvas texture. Negating y there stores the canvas
vertically mirrored relative to the reference and gradient textures.

That was a real bug in this port, and an instructive one. `error.comp` was
comparing the reference against a mirrored copy of the canvas and
`trace.comp`'s termination test was reading the wrong pixel, yet the output
image looked plausible — because `writePng` flipped again on the way out, and
because both errors inflate detail rather than destroying it. It cost 5x the
strokes and produced a visibly busier painting, but nothing about the image said
"vertical flip".

What found it was a number, not an eye: the layer-1 cell error was 52.3 where
`worker.js` reported 14.25. An independent Python computation of the ideal error
map — two Gaussian blurs of the source at consecutive sigmas, in the same Lab
encoding — put ~0% of cells above threshold 50 against the pipeline's 38%, which
localised the fault to the canvas rather than the error metric.

So: display orientation is applied at the edges (`blitToScreen` flips for the
screen; `readCanvas` hands back texel rows in order and `writePng` does not
flip), never inside the pipeline. And when comparing against the reference
implementation, compare intermediate quantities per layer, not just the picture.

## Relaxation (optional, `--relax`)

Hertzmann 2001 on top of the greedy result. Three stages, and one architectural
change to support them.

The change: the greedy phase paints a layer in several passes that all reuse the
same stroke buffers, so by the end of a frame nothing remembers the strokes.
Relaxation needs them all, so `pool.comp` collects every stroke into a
**frame-wide pool** as it is painted — `trace.comp` reserves a slot range per
pass with a single atomic on the pool counter, and `pool.comp` copies headers
and polylines into it. The pool holds geometry, not seeds, because the paper
perturbs control points rather than re-running the tracer; that is also why
`relax.comp` needs no gradient field and no per-layer reference image.

| Stage | File | What it does |
|---|---|---|
| 8 | `pool.comp` | Appends a pass' strokes to the pool; also writes the indirect draw for a whole-pool repaint |
| 9 | `relax.comp` | One thread per stroke: proposes trial moves, keeps the best |
| 10 | `energy.comp` | Workgroup-wise reduction of the global appearance energy |

`energy.comp`'s partial-sum buffer is sized for two floats per workgroup and is
shared with `etf.comp`'s coherence reduction, which needs a sum and a count. The
two are never in flight together.

Each iteration runs `relax.comp`, then `repaintFromPool()` — there is no
incremental undo of a blended stroke, so moving one means repainting the canvas
from the ground up. That repaint is why `layUnderpaint` caches the ground in
`m_underTex`: recomputing the underpaint Gaussian every iteration would
otherwise dominate the cost.

Two design notes that are load-bearing, both written up with measurements in
`docs/relaxation.md`:

- **The area term must not be formulated so that both terms scale with area.**
  `E = area * meanError + w_area * area` makes `w_area` a constant offset and
  shrinking free. The fix is to charge for the pixels a shrinking stroke stops
  covering, which gives `score = A * (Em + w_area - Eg)`.
- **`Eg`, the error of "the paint underneath", is measured against the frame's
  ground, not the current canvas.** The canvas already contains the stroke being
  judged, so measuring there is circular and reports every stroke as redundant.
  With that version removal deleted 94% of the strokes.

`relax.comp` reads only the source and the ground, neither of which relaxation
changes, so the sub-pass machinery that once existed to serialise canvas reads
is no longer needed for correctness (`relaxSubPasses` defaults to 1) and the
repaint happens once per iteration rather than once per sub-pass.

## Stroke identity is hashed from position

A stroke's colour jitter, brush tile variant and angle jitter come from a hash
of **where the stroke is**, never from the frame counter (`frameSalt()` returns
0 unless `--jitter-per-frame` asks otherwise). `worker.js` uses `Math.random()`
for these, which re-rolls every stroke on every frame; on video that guarantees
the paint boils even where the picture is identical. Measured on a near-static
clip, repainting every frame: 12.49 mean frame-to-frame change with the frame
counter in the hash, 4.33 without.

`trace.comp` hashes the seed's **pixel position** rather than its thread index
for the same reason: the index is a compaction order that shifts whenever a
neighbouring cell passes or fails the threshold, so an index-based hash would
re-roll the angle jitter of a stroke that had not moved at all.

Note what this does *not* buy: the renders are still not bit-reproducible,
because `seeds.comp` appends through `atomicAdd` and the resulting buffer order
— hence the compositing order — varies with GPU scheduling, which then feeds
back into later layers through the canvas. Run-to-run stroke counts move by
about 0.3%. Making that deterministic would need a prefix-sum compaction instead
of atomics.

## Jobs and the GUI (`src/jobs.cpp`, `src/filedialog.cpp`)

The video, batch and frame-sequence pipelines live in `jobs.cpp`, not in
`main()`. They were inline until the GUI needed to run the same work; a second
copy would have drifted from the CLI's within a release.

Each job takes a `ProgressFn` that reports a frame and **returns false to
cancel**. That one signature is what lets the GUI stay responsive without
threads: its callback pumps `glfwPollEvents`, draws a progress panel and swaps
buffers between frames. A worker thread would need a second GL context and
sharing, which is not worth it when the job is GPU-bound anyway and the context
is single-threaded.

Jobs take `TuningParams` **by value**: each advances its own frame counter, and
a job must not disturb the interactive session's.

`filedialog.cpp` wraps `IFileOpenDialog` / `IFileSaveDialog` rather than the
legacy `GetOpenFileName` — no `MAX_PATH` truncation, and folder picking is the
`FOS_PICKFOLDERS` flag rather than a separate shell-browse API. It needs no new
dependency: MSVC already links `ole32` and `shell32` by default. Non-Windows
builds get stubs that return empty, so callers only ever check for empty.

GUI state is one `Gui` struct, so the drop handler, the four picker buttons and
the export button cannot disagree about what is loaded or where it is going.
`acceptPaths()` is the single classifier — several files or a folder means
batch, a video extension means video, anything else is tried as a still — and
both drag-drop and the pickers go through it.

For video the GUI decodes **one frame** as the preview (`media::readFrameAt`,
which seeks with `-ss` before `-i` rather than streaming) so the sliders act on
real footage. Scrubbing re-seeks on slider *release*, not during the drag, or
every UI frame would spawn an ffmpeg process.

## Video and batch (`src/media.cpp`)

ffmpeg runs as a child process and frames cross a pipe as raw RGBA, which is
already the format `setSource()` and `readCanvas()` use, so there is no
conversion on this side. Linking libav* instead would add a large build
dependency and a container/codec matrix to maintain, for a renderer whose job is
the painting.

`media::probe` shells out to ffprobe for size, frame rate, frame count and
whether there is an audio stream, printing one field per line so the parse needs
no JSON. The writer takes the source file as a second ffmpeg input and maps
`0:v` plus `1:a?` to carry audio across; the trailing `?` makes the audio
mapping optional so a silent input still encodes. Reads loop until a frame is
whole, because a pipe can return a short read without being at end of stream.

Batch mode resets temporal state per image and allows the size to change between
files; an unreadable file is skipped and counted rather than fatal.

## Beyond the port

Two stages have **no counterpart in worker.js**. Both are off by default, and
both must stay that way: the port's whole verification method is comparison
against the web renderer, and a stage the web renderer does not have cannot
participate in it. With `--etf 0 --flow 0` the output is the web port's, within
the usual ~2% atomics noise.

Because neither can be checked against a reference implementation, each ships
with its own measurement instead. A picture that merely looks different is not
evidence that a filter works, and both of these change the picture whether they
are correct or not.

### Edge Tangent Flow (`--etf`, `shaders/etf.comp`)

Kang, Lee & Chui 2007. An edge-aware smoothing of the per-layer tangent field:
weak pixels defer to strong neighbours, nothing is smoothed towards a
perpendicular flow, and a tangent and its negation are treated as one direction.
A Gaussian can express none of those, which is why the structure-tensor path
does not solve the same problem.

It is a **direction filter only** — the magnitude channel is carried through
untouched and the output is rescaled to the input's length, so `trace.comp`'s
gradient floor and the fc filter's magnitude weighting at curvature < 1 both see
exactly what they saw before. Keep that property if the stage is modified.

Verified by flow coherence (`--etf-log`), which rises monotonically with
iterations in every layer, and most in the finest layer where the plain Sobel
field is noisiest: 0.83 -> 0.95 at three iterations, against 0.96 -> 0.98 for
the heavily blurred coarse layer. Numbers and method in `docs/etf.md`.

### Optical flow advection (`--flow`, `shaders/flow.comp`)

Pyramidal Lucas-Kanade, used by the temporal path to carry the painting along
the motion. Without it, `--temporal-diff` does nothing useful on a moving
camera: every pixel changes, so every cell repaints. Measured on a synthetic pan
with exactly known displacement, there is **no threshold at which the no-flow
path is stable** — raising it makes matters worse, because the paint freezes in
place while the content moves out from under it. With flow the same knob works,
4.8x better at matched settings.

What it advects is the **canvas, not a stroke list**: this architecture carries
an image between frames, not strokes, so nothing here re-orients a stroke or
moves it as an object. `SBR_SLAB` in `params.h` is vestigial and does not
describe the implementation.

The cost of advecting an image is resampling, every frame the paint survives, and
it compounds invisibly: bilinear warping destroyed 89% of the painting's
sharpness over eleven frames while every frame-to-frame stability number looked
excellent. Catmull-Rom resampling and an exact-copy fast path for unmoved pixels
bring that back to 44% retained, and make a still camera exactly lossless.
Sustained motion still softens the painting. `docs/flow.md` has the tables.

## Not ported

Palette quantisation (`buildPalette`), the salience and manual detail maps
(`computeSalience`, `buildDetailMap`), and the Litwinowicz / Haeberli / pencil /
neural algorithms. `paintHertzmann` only.

Temporal coherence follows the web's rule by default — a cell keeps last frame's
paint unless its source moved by more than `frameDiffThreshold`. `--flow` adds
Lucas-Kanade advection *on top of* that rule rather than replacing it: the
carried canvas is warped along the motion first, so the threshold test asks
whether the subject changed rather than whether the pixel did. At `--flow 0` the
behaviour is the web's, unchanged.

## OBS live bridge

The offline renderer and OBS filter share one `Pipeline`. Live Spout
frames enter through `Pipeline::setSourceTexture`, which preserves the previous
source for temporal painting and copies the new input entirely on the GPU.
`Pipeline::canvasTexture` is handed to Spout2 for publication. A hidden GLFW
window owns the OpenGL context; the OBS plugin does not run the painting
shaders on the OBS Direct3D device.

`obs/paintify-filter.cpp` registers an OBS effect filter. On the graphics thread
it captures the upstream target into a BGRA texture, sends it through SpoutDX,
opens the renderer's shared output texture, and draws that texture in the filter.
It falls back to OBS's original-source path until a matching painted texture is
available. The plugin runs one renderer process per filter instance, with names
derived from the OBS PID and a monotonic instance counter. User controls are
snapshotted under a mutex; a change restarts the corresponding renderer on the
graphics thread and discards the old temporal state. Filter destruction stops
the child and releases GPU resources. The renderer caps painting at 12 fps by
default; OBS holds the last painted texture between updates.
