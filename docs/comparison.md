# Verifying gpu-sbr against PainterlyImageCreatorWeb

The point of this renderer is to reproduce `worker.js`'s `paintHertzmann`
output, so "looks about right" is not a sufficient test. This is how the two
are compared and what the numbers currently are.

## Method

`worker.js` runs unmodified in Node under a `vm` context: `importScripts` is
stubbed, `worker.js` and `brush-texture.js` are loaded into the same context,
and `paintify` is called directly. The functions that matter
(`computeErrorMap`, `chooseBestInCell`, `renderStrokeSolid`) are wrapped in
that context to count per-layer cells, threshold passes, drawn strokes and mean
points per stroke, without editing the upstream repo.

gpu-sbr reports the same quantities: `--debug-cells` prints each layer's
cell-error distribution, and the stats buffer carries seeds, drawn strokes and
total points per (layer, pass).

Both renderers get the same 960x1280 image and the `impressionist` preset.

**The single most useful control is running the web renderer twice.** It uses
`Math.random()`, so two runs differ. That difference is the noise floor: no
port can agree with the web version more closely than the web version agrees
with itself, and a residual at that level is not evidence of a defect.

## Per-layer agreement

Radii 8, 4, 2; threshold 50; gridFactor 1.

| | cells | web cell error | gpu cell error | web drawn | gpu drawn |
|---|---|---|---|---|---|
| layer 0 (r=8) | 19,200 | 0.00 | 0.00 | 19,015 | 19,010 |
| layer 1 (r=4) | 76,800 | 14.25 | 14.3 | 2,408 | 2,593 |
| layer 2 (r=2) | 307,200 | 14.79 | 14.6 | 11,300 | 11,287 |
| **total** | | | | **32,723** | **32,890** |

Layer 0's error is exactly zero in both, and necessarily so: the underpaint is
the source blurred at `radii[0] * 0.5`, which is also layer 0's own reference
image. The layer paints anyway because `isFirstLayer` bypasses the threshold.

## Whole-image agreement

RMS difference in RGB, raw and after a Gaussian blur. Blurring separates
"different individual strokes landed in different places", which is expected
and is what the noise floor measures, from "the images differ structurally".

| | raw | blur 4 | blur 12 |
|---|---|---|---|
| web vs web (noise floor) | 19.66 | 11.00 | 6.09 |
| **web vs gpu-sbr** | **19.41** | **10.51** | **5.75** |
| web vs gpu-sbr, before the y-flip fix | 22.99 | 13.45 | 8.64 |

(Re-measured after stroke identity moved from `Math.random()`-style per-frame
jitter to a position hash — see the note below. The match is unchanged, and
sits just inside the noise floor.)

Mean RGB: web run 1 `54.39 49.68 49.63`, web run 2 `53.80 49.11 48.91`,
gpu-sbr `54.23 49.51 49.32` — the port falls between the two web runs.

With impasto at the web's Medium (strength 0.3, light 0.3): blur-12 RMS 5.95,
mean RGB web `52.65 48.15 48.13` against gpu `52.46 47.92 47.88`.

gpu-sbr agrees with the web renderer as closely as the web renderer agrees with
itself. There is no measurable structural difference left at this image size and
preset.

## Cost

| Image | Drawn strokes | gpu-sbr | worker.js (1 CPU thread) |
|---|---|---|---|
| 960 x 1280 | 32K | 2.40 ms | 173,000 - 215,000 ms |
| 1920 x 1080 | 31K | 3.74 ms | — |
| 2880 x 3840 | 245K | 10.90 ms | — |

The 960x1280 row is the like-for-like pair: roughly 72,000x, though the CPU
figure is a single-threaded Node run and not a fair picture of what the web app
does with workers.

## A deliberate divergence: stroke identity

`worker.js` draws a stroke's colour jitter, brush-tile variant and angle jitter
from `Math.random()`, so they are re-rolled for every stroke on every frame.
gpu-sbr hashes them from the stroke's **position** instead. On a single still
this is simply a different random stream and the statistics above are unaffected
— the mean RGB still lands between the two web runs. On video it is the
difference between paint that boils and paint that sits still: repainting every
frame of a near-static clip gives a mean frame-to-frame change of 12.49 with the
frame counter folded into the hash and 4.33 without.

`--jitter-per-frame` restores the web behaviour. Note that `brush-texture.js`
already hashes its tile variant from the seed position for exactly this reason
("never Math.random() — so video frames stay stable"), so this extends a choice
the upstream author had already made for one of the three.

## Two bugs this comparison caught

Neither was visible in the output image, which is the argument for measuring
rather than eyeballing.

**The canvas was stored vertically mirrored.** `stroke.vert` negated y to put
image row 0 at the top of the framebuffer, but window y 0 is the *bottom* row of
the attached texture — so the canvas texture ended up flipped relative to the
reference and gradient textures, which the compute stages address by texel.
`error.comp` was therefore comparing the reference against a mirrored copy of the
canvas, and `trace.comp`'s termination test was reading the wrong pixel.

The output looked plausible because `writePng` flipped again on the way out, and
because both errors inflate detail rather than destroying it. The measurable
symptoms were a layer-1 cell error of 52.3 against the web's 14.25, and 178,000
drawn strokes against the web's 33,000. An independent check settled it: a
Python computation of the ideal error map — two Gaussian blurs of the source at
consecutive sigmas, converted to the same Lab encoding — puts roughly 0% of cells
above threshold 50, not 38%.

**Stroke length needs intra-layer feedback.** `worker.js` paints one stroke at a
time, and `makeCurvedStroke` stops once the canvas already matches the reference
better than the stroke's own colour would. Every stroke therefore sees the paint
of every earlier stroke in its own layer. Painting a whole layer in one parallel
pass removes that entirely, and strokes come out too short (mean 5.44 points
against the web's 7.03). Splitting each layer into hash-scattered passes restores
it:

| passes per layer | mean points/stroke | GPU |
|---|---|---|
| 1 | 5.44 | 1.23 ms |
| 2 | 6.51 | 1.43 ms |
| 4 | 6.82 | 1.74 ms |
| **8 (default)** | **6.96** | **2.38 ms** |
| 16 | 7.01 | 3.81 ms |
| 32 | 7.05 | 7.61 ms |
| web | 7.03 | — |

Eight passes lands within 1% at under half the cost of 16. `--passes` exposes it.

## Relaxation is verified separately

`--relax` implements Hertzmann **2001**, which the web version does not have at
all, so there is nothing to compare it against. It is checked against its own
objective instead: `energy.comp` reduces the global appearance energy and
`--relax-log` prints it per iteration. Two real bugs in it were caught that way,
both invisible in the output image. See `docs/relaxation.md`.

## Reproducing

The harness lives outside this repo (it reads `worker.js` from a sibling
checkout). It needs Node and, for the image encode/decode and the metrics,
Python with Pillow, NumPy and SciPy. The shape of it:

```sh
# raw RGBA in, raw RGBA out, PIL on either side
python -c "from PIL import Image; im=Image.open('assets/test.jpg').convert('RGBA'); \
           open('src.bin','wb').write(im.tobytes())"
node web_render.mjs impressionist 0 0 0.45      # ~3 minutes
build\gpu-sbr.exe --headless --in assets\test.jpg --out gpu.png \
                  --preset impressionist --passes 8
```

`probe.mjs` is worth keeping in mind as a pattern: it calls a single upstream
function (`makeCurvedStroke`) in the vm across a parameter sweep and answers a
targeted question in under a second, instead of paying for a full render. That
is how the curvature-0 degeneracy below was found.

## Known upstream quirk

At `curvature: 0.0`, `makeCurvedStroke` blends
`0 * gradientPerp + 1 * lastDir` on the first step, where `lastDir` is still
`(0, 0)`. The direction is degenerate, the walk breaks immediately, and the
stroke has one point — which `renderStrokeSolid` declines to draw. The web's own
`pointillist` preset sets curvature 0, so it paints nothing but the underpaint.
Verified directly: 200/200 strokes come back with a single point at curvature 0,
0/200 at 0.25 and above.

gpu-sbr seeds `lastDir` from the gradient on the first step instead. That is
bit-identical for every curvature above 0 — the blend reduces to the gradient
direction either way — and turns curvature 0 into the straight dabs the preset
is asking for.
