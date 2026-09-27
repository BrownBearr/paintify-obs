# Paint by relaxation

Hertzmann 2001, "Paint By Relaxation", on the GPU. The greedy 1998 algorithm
places every stroke once and never looks at it again; relaxation treats the
painting as an energy minimisation and improves the strokes it already has.
`--relax <n>` turns it on; 0 (the default) is the plain 1998 result.

    E = E_app + E_area
    E_app  = sum over pixels of || Lab(canvas) - Lab(source) ||
    E_area = w_area * sum over strokes of area(stroke)

The operations are the paper's: move a control point, translate the stroke,
rotate it, shorten it, change its width, recolour it. Note that the paper
perturbs the **control points of existing strokes** rather than re-running the
tracer, which is why `relax.comp` needs no gradient field and no per-layer
reference image — only the source, the stroke pool, and the frame's ground.

## What it looks like

Relaxation makes the stroke work bolder and more deliberate: fewer hesitant
marks, larger confident ones that follow the form. On the test image, 4
iterations lower E_app by 25–33%.

## Architecture

The greedy phase paints a layer in several passes, all reusing the same stroke
buffers, so by the end of a frame nothing remembers the strokes. Relaxation
needs them all, so a **frame-wide pool** (`pool.comp`) collects every stroke as
it is painted: `trace.comp` reserves a slot range per pass with one atomic, and
`pool.comp` copies the headers and polylines into it.

Each iteration then:

1. runs `relax.comp`, one thread per pooled stroke, proposing
   `--relax-candidates` trial moves and keeping the best;
2. repaints the canvas from the pool (`repaintFromPool`) — there is no
   incremental undo of a blended stroke, so moving one means painting the whole
   canvas again from the ground up;
3. optionally measures the global energy (`energy.comp`) for the log.

The repaint is cheap because the ground is cached: recomputing the underpaint
Gaussian every iteration would otherwise dominate.

Strokes are removed by setting `vertexCount` to 0, which `stroke.vert` skips.
The slot stays put so pool indices remain stable across iterations.

## The two things that were wrong, and why they were worth finding

Both produced plausible-looking output while being measurably broken, which is
the argument for logging the energy rather than trusting the picture.

### 1. The area weight did nothing at all

The obvious way to localise the energy is to score a candidate stroke as

    E = area * meanFitError + w_area * area

This is degenerate. **Both** terms are proportional to area, so `w_area` only
adds a constant to the error and shrinking a stroke is free — the optimiser
drives every stroke toward nothing, and the weight that was supposed to control
economy has no effect on the outcome. Measured across `--relax-area` 0 → 15,
the global energy moved by 0.4% and the stroke geometry not at all.

What the global energy has and that version lacked is **the cost of the pixels a
shrinking stroke stops covering**. Globally `E_app` is a sum over *all* pixels,
so uncovering some does not drop them from the sum — it hands them back to the
paint underneath. Charging for that, with `A` the candidate area, `A0` the
current area, `Em` the candidate's mean fit error and `Eg` the mean error of the
paint underneath:

    E = A * Em + w_area * A + (A0 - A) * Eg
      = A * (Em + w_area - Eg) + A0 * Eg

The last term is constant per stroke, so ranking candidates only needs

    score = A * (Em + w_area - Eg)

which behaves the way the paper's energy does. A stroke that fits its footprint
better than the paint underneath (`Em + w_area < Eg`) lowers the score by
growing; one that fits worse lowers it by shrinking; and `w_area` moves that
break-even point. Removal is the same test with no area at all.

### 2. "What is underneath" must not mean the current canvas

`Eg` is measured against the frame's **ground** — the underpaint the canvas
started from — not against the current canvas. Measuring it on the current
canvas is the natural-looking choice and is badly circular: that canvas already
contains the stroke being judged, so it reports "the paint here is fine"
precisely because the stroke put it there, and every stroke in the painting
looks redundant.

With that version, `--relax-remove 1.0` deleted 94% of the strokes, and on a
blank canvas `E_app` rose from 14.9 to 47.5 — it was removing exactly the
strokes doing the work, and the energy log is what made that obvious.

The ground is exact for the first layer and pessimistic for later ones (it
ignores intermediate layers), which errs toward keeping strokes — the safe
direction.

## The underpaint is part of the economics

A consequence worth knowing: with `--underpaint blur` the ground is already a
good fit to the source, so strokes are worth less and relaxation trims harder.
With a blank or flat ground, coverage is valuable and strokes grow. The paper
starts from a blank canvas. Measured at 4 iterations on the test image:

| ground | area weight | removal | live strokes | coverage | E_app |
|---|---|---|---|---|---|
| blur | 0 | off | 32,487 | 14.7x | 14.94 → 10.38 |
| blur | 2 | off | 32,787 | 12.9x | 14.88 → 10.39 |
| blur | 2 | 1.0 | 12,279 | 1.6x | 14.91 → **9.52** |
| none | 0 | off | 33,765 | 27.2x | 14.86 → 11.86 |
| none | 2 | 1.0 | 33,946 | 27.4x | 14.82 → 11.80 |

The third row is the interesting one: removal reaches the *lowest* appearance
error of any configuration while using roughly a tenth of the paint. That is
what the area term is for.

## Reading the energy log

`--relax-log` prints `E_app` after each iteration. **It is only expected to fall
monotonically at `--relax-area 0`.** With a positive area weight, relaxation is
deliberately spending appearance to buy economy, so `E_app` can rise while the
real objective `E_app + w_area * sum(area)` still falls. The log prints the area
term alongside so the objective can be checked.

Convergence, `--relax-area 0`:

| iterations | E_app | GPU (960x1280) |
|---|---|---|
| 0 | 14.90 | 2.4 ms |
| 1 | 11.41 | — |
| 2 | 11.23 | 24.7 ms |
| 4 | 11.10 | 42.8 ms |
| 8 | 11.04 | 79.1 ms |

Most of the gain is in the first iteration, and almost all of it by the fourth;
2–4 is the useful range. The first iteration's jump is largely the recolour
operation, which is solved in closed form rather than searched: the
coverage-weighted mean of the source under a stroke is the least-squares optimal
colour, and greedy strokes take the colour at their seed point instead.

## Known approximations

- **Trial moves are scored per stroke, not globally.** The paper re-renders to
  evaluate how a move interacts with the strokes painted over it, which is
  inherently serial. A move that helps this stroke and hurts an overlapping one
  is not caught here. `energy.comp` reports the true global `E_app` per
  iteration so this stays checkable rather than assumed.
- **`Eg` ignores intermediate layers** (see above).
- **Lengthening is not a move.** Shortening is; growth past a stroke's traced
  end would need extrapolation, which walks it off the feature it was
  following. New coverage comes from the greedy add pass instead.
- **The pool is capped** at `SBR_POOL_MAX` (262,144 strokes, ~71 MB). Above
  that, relaxation is skipped for the frame with a message rather than silently
  painting from an incomplete pool — a repaint from a truncated pool would lose
  strokes the greedy phase did paint.
