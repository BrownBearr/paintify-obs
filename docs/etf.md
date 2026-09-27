# Edge Tangent Flow

Kang, Lee & Chui 2007, *Coherent Line Drawing*, section 3, applied to the
per-layer direction field. `--etf <n>`, off by default.

This is the first stage in gpu-sbr with **no counterpart in worker.js**, so it
cannot be verified against the web renderer the way the rest of the port was.
That is why it is opt-in and why it ships with its own measurement rather than
a screenshot.

## What it is for

Both existing flow fields have the same two failures:

- the direction wobbles pixel to pixel exactly where the structure is
  strongest, because Sobel is a local operator on a noisy signal;
- in flat regions the direction is whatever the noise says, because a Gaussian
  smooths *across* an edge as happily as along it.

The structure-tensor path (`--tensor-sigma`) helps with the first by smoothing
the tensor rather than the vectors, which at least stops opposite-signed
gradients cancelling. It does not help with the second: it is still an isotropic
Gaussian, and it has no notion that a weak pixel should defer to a strong one.

ETF is an edge-*aware* smoothing of the tangent field. Each pixel takes a
weighted mean of its neighbours' tangents over a disc of radius `r`:

| term | definition | what it does |
|---|---|---|
| `w_s` | 1 inside the disc, 0 outside | the neighbourhood |
| `w_m` | `(1 + tanh(eta * (g(y) - g(x)))) / 2` | a weak pixel is pulled towards its strong neighbours, never the reverse |
| `w_d` | `abs(dot(t(x), t(y)))` | nothing is smoothed towards a perpendicular flow |
| `phi` | `sign(dot(t(x), t(y)))` | a tangent and its negation are the same direction |

A Gaussian has none of those three terms. That is the whole difference.

## It is a direction filter and nothing else

The magnitude channel is carried through untouched, and the output vector is
rescaled to the input's length. Every downstream consumer therefore sees exactly
the quantity it saw before:

- `trace.comp`'s gradient-magnitude floor (`grad.z * stepLen < 1e-3`);
- the fc filter's weighting of `nd` against a unit `lastDir`, which at
  curvature < 1 is part of the curve's shape and would change silently if the
  field were normalised here.

This is what lets ETF be switched on and off without changing anything else
about the render, and it is worth preserving if the stage is ever modified.

## Measuring it

An algorithm with no reference implementation needs a number, or "it looks
different" becomes the only evidence. The number here is **flow coherence**: the
mean of `abs(dot(t(x), t(y)))` between each pixel's tangent and its eight
neighbours', over the whole field. `--etf-log` prints it per layer, before and
after.

The floor is **2/pi ~= 0.637**, which is what a uniformly random field scores —
so that, not zero, is what a reading should be compared against.

`--etf-log` on `assets/test.jpg`, radii 8/4/2, r = 5:

| iterations | layer 0 (r 8) | layer 1 (r 4) | layer 2 (r 2) | frame |
|---|---|---|---|---|
| 0 | 0.9608 | 0.9034 | 0.8296 | 2.3 ms |
| 1 | 0.9689 (+0.8%) | 0.9283 (+2.8%) | 0.8696 (+4.8%) | 5.5 ms |
| 2 | 0.9776 (+1.8%) | 0.9523 (+5.4%) | 0.9120 (+9.9%) | 9.3 ms |
| 3 | 0.9838 (+2.4%) | 0.9702 (+7.4%) | 0.9475 (+14.2%) | 12.2 ms |
| 5 | 0.9904 (+3.1%) | 0.9868 (+9.2%) | 0.9827 (+18.5%) | 17.5 ms |

Two things in that table are the actual evidence, beyond "the number went up":

- **It rises monotonically**, in every layer, at every iteration count. A filter
  that was merely perturbing the field would not.
- **The gain is inversely proportional to how blurred the layer already is.**
  The radius-8 layer is computed on a heavily Gaussian-blurred reference and
  starts at 0.96, with almost nothing left to win. The radius-2 layer starts at
  0.83 and gains 14%. That is precisely where the plain Sobel field is noisiest,
  and it is the behaviour the method predicts — not something that had to be
  tuned for.

## Effect on the painting

At `--etf 3`, strokes run slightly longer (6.99 -> 7.06 mean points) and roughly
7% fewer are placed (32.2K -> 30.2K seeds), concentrated in the fine layers
(10.7K -> 9.4K at radius 2). A more coherent coarse field means coarse strokes
follow structure further, leaving less residual error for the fine layers to
correct. That chain — better field, longer strokes, less residual, fewer fine
strokes — is consistent end to end, which is a second check that the stage is
doing what it claims.

## Cost

Roughly 3 ms per iteration per frame at 960x1280 across three layers, on a
3060 Ti. The kernel is a disc, so cost is quadratic in the radius and linear in
both the iteration count and the layer count.

2-3 iterations is where the paper says it converges, and the table above agrees:
the fine layer is at 0.95 by iteration 3 and the remaining iterations buy
progressively less.

The full 2D disc kernel is used rather than the separable approximation that
several published implementations substitute. The weights are not separable
(`w_d` depends on both endpoints), the separable version is therefore an
approximation of an approximation, and this project's rule is that fidelity wins
unless the two are provably equivalent. If the cost ever matters more than that,
the separable form is the first thing to try.
