# Optical flow advection

Dense pyramidal Lucas-Kanade flow, used to carry the painting along the motion
instead of leaving it pinned to the pixel grid. `--flow <levels>`, off by
default, and it does nothing unless `--temporal-diff` is also above zero.

## The problem it solves

Temporal mode carries the previous frame's **canvas** forward and repaints only
the cells whose source moved by more than `frameDiffThreshold`. On a static
camera that is nearly perfect. On a pan it was close to useless:

- every pixel's source has moved, so every cell clears the threshold;
- so every cell repaints, with fresh jitter;
- so the painting is rebuilt from scratch each frame even though the picture
  only translated.

Measured on a synthetic 6 px/frame pan (below), the flow-off path repainted
20,452 strokes per frame out of a possible ~21,000. `--temporal-diff` was, in
effect, a knob that did nothing on moving footage.

## What is advected

**The canvas, not a stroke list.** This is the honest limitation and it decides
everything else about the design.

The architecture carries an image between frames — there is no persistent
stroke pool spanning frames (the relaxation pool is rebuilt every frame, and
`SBR_SLAB` in `params.h` is vestigial). So this stage advects *the image of the
painting*. It does not:

- re-orient a stroke to the new flow field;
- move strokes as objects, so a stroke straddling a motion boundary is torn
  rather than split;
- preserve stroke identity across a frame in any form a later stage could use.

A true Hays & Essa 2004 implementation would advect stroke *control points* and
re-rasterise. That needs a cross-frame stroke pool, which is a much larger
change to the architecture than this is.

## Method

`shaders/flow.comp`. Pyramidal Lucas-Kanade, coarse to fine, with a
component-wise 3x3 median between levels.

**Convention.** The stored vector `d` is a *backward* displacement in full
resolution pixels, defined by `prev(x + d(x)) ~= cur(x)`. Every consumer samples
the previous frame at `x + d` and nothing has to remember a sign. `d` is zero
when nothing moved, which makes "flow off" and "flow computed but static" the
same code path.

**Resolution.** The field is computed and stored at a quarter of the image size.
A field used to steer a warp only has to be smooth and roughly right, and
quarter resolution cuts the work 16x; the consumers sample it with a normalised
coordinate, so the bilinear upsample is free. That also fixes where the pyramid
stops: the LK window is spaced one level pixel apart, and at level 2 (the 4x
downsample) that spacing is exactly the flow grid's own. Refining below level 2
would measure a window narrower than the gap between flow samples.

**Gradients are taken on the current frame**, not on the warped previous one.
That is the usual approximation — at convergence the two match — and it keeps
them independent of `d`. Coarse-to-fine is what keeps `d` small enough for it to
hold.

Three details are load-bearing, each for a reason that is invisible in any
single frame:

- **The eigenvalue floor.** Below `MIN_EIGENVALUE` the 2x2 system is degenerate:
  a flat patch, or a straight edge where motion along the edge is unobservable.
  Solving it anyway produces a large, confident, wrong vector. For advection
  that is much worse than admitting ignorance — a wrong vector smears paint
  across the canvas, whereas a zero leaves the paint where it was and lets the
  ordinary difference test repaint it.
- **The per-iteration clamp** of one level pixel. The linearisation is only
  valid for sub-pixel steps; without it a single bad window throws a vector big
  enough to tear a hole in the warped canvas.
- **A median, not a box, between levels.** Averaging does not remove an outlier,
  it spreads it over nine cells — and the next finer level refines whatever this
  one produced, so a smeared outlier is a bad *starting point* over a wider
  area. Switching from a box to a component-wise median cut the field's maximum
  magnitude from 44 px to 25-34 px against a ground truth of 6.

## Verification

A pan is the case where a raw frame-to-frame difference says nothing: the
content really did move, so a large difference is correct. The question is
whether the *paint* moved with it.

`tools/` has no harness for this; the measurement used here is a synthetic pan
with an exactly known displacement, so the metric needs no flow estimate of its
own:

1. crop an 800x1000 window out of `assets/test.jpg` at `x0 = 64 + 6k` for 12
   frames, so the true backward flow is exactly `(+6, 0)` px everywhere;
2. paint the sequence with `--frames`;
3. compare `painted[k](x)` against `painted[k-1](x + 6)` over the overlap.

The metric reads **0.000** on the source frames themselves and **0.000** on a
static-camera sequence, so it has no offset of its own.

**The flow estimate is correct**: `--flow-log` reports mean `d = (6.013, -0.047)`
against a ground truth of `(6, 0)`.

**Motion-compensated frame-to-frame change**, 12-frame pan, `--flow 4`:

| `--temporal-diff` | flow off | flow on | strokes repainted/frame (on) |
|---|---|---|---|
| 12 | 14.30 | 12.61 | 5,784 |
| 24 | 14.44 | 8.27 | 2,319 |
| 48 | 14.46 | **3.04** | 855 |
| 250 (nothing repaints) | 16.49 | **0.82** | 0 |

The flow-off column is the result worth reading twice. Raising the threshold
does not help it at all, and at the limit it gets *worse* — because the paint is
then frozen in place while the content pans out from under it. **There is no
setting at which the no-flow path is stable on a pan.** With flow, the same knob
works: 4.8x better at matched settings, 20x at the advection limit.

## What it costs: resampling

Advecting an image means resampling it, every frame it survives without being
repainted, and resampling is lossy. This is the price of carrying a canvas
rather than a stroke list, and it is large enough to need managing.

Sharpness here is the variance of the Laplacian, measured on frames 0, 6 and 11
of the pan with nothing repainting (`--temporal-diff 250`) — the worst case:

| resampling | frame 0 | frame 6 | frame 11 | retained |
|---|---|---|---|---|
| bilinear | 652 | 131 | 75 | 11% |
| Catmull-Rom | 657 | 358 | 291 | 44% |

Bilinear is a low-pass filter, so eleven successive applications destroyed 89%
of the image's high frequencies. Every frame-to-frame stability number looked
excellent while the painting dissolved — which is exactly the kind of failure a
single-frame check cannot see, and the reason this table exists.

Catmull-Rom has negative lobes that restore what a resample takes out; it is
what temporal anti-aliasing uses for this same job. It does not make the warp
lossless, only slow enough to lose that the ordinary repaint threshold refreshes
paint faster than the warp degrades it.

Two mitigations are in the shader:

- **Catmull-Rom** instead of bilinear, with the result clamped to 0..1 — the
  negative lobes overshoot at a hard stroke edge, and an out-of-range value here
  is carried into the next frame's warp and amplified.
- **An exact-copy fast path** where the flow is below a hundredth of a pixel.
  This makes a still camera *exactly* lossless: sharpness measured 661.9 on
  every frame of a 12-frame static sequence, byte for byte, with flow on. Most
  footage is mostly still most of the time, and without this an unconditional
  warp would slowly dissolve a background that never needed touching.

The residual limitation stands and is not fixed: **sustained motion softens the
painting**, by roughly half over ten frames of continuous panning with a high
repaint threshold. Lower thresholds trade that back for stability. Advecting
stroke geometry instead of pixels is the real fix and is not implemented.

## Cost

About 0.6-1.4 ms/frame at 800x1000 on a 3060 Ti (1.82 -> 2.43 ms total at
`--temporal-diff 12`). Each extra level doubles the largest motion that can be
tracked, so `--flow 4` covers roughly 32 px of displacement per frame. It is
only ever computed on the temporal path, so stills pay nothing.
