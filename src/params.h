#pragma once
#include <cstdint>
#include <vector>

// Stroke-buffer capacities. A layer whose seed grid asks for more strokes than
// this is painted in several chunks, so these bound VRAM rather than output.
static constexpr uint32_t SBR_MAX_STROKES = 200000u;   // per chunk
static constexpr uint32_t SBR_MAX_VERTS   = 33u;       // polyline points, matches MAX_VERTS
static constexpr uint32_t SBR_MAX_LAYERS  = 8u;
// Temporal mode keeps one persistent seed slab per layer, double-buffered so a
// frame reads last frame's strokes while it writes this frame's survivors.
static constexpr uint32_t SBR_SLAB        = 65536u;
// Frame-wide stroke pool, for relaxation. Holds every stroke of a frame, not
// just one pass': 32 B/header plus 33 points x 8 B, so 256K strokes is ~71 MB.
static constexpr uint32_t SBR_POOL_MAX    = 262144u;

// Mirrors the `Params` uniform block in shaders/common.glsl (std140).
// Every member is a float so the std140 layout is a flat array of vec4s --
// keep it that way when adding fields, and add them in groups of four.
//
// Names and default values track PainterlyImageCreatorWeb's PRESET_DEFAULTS
// (main.js) and the sliders that feed `paintify`, so a setting tuned in the web
// UI can be typed in here and mean the same thing.
struct TuningParams {
    // --- image (filled in by the pipeline) ---
    float imgW = 0.f, imgH = 0.f;
    float texelW = 0.f, texelH = 0.f;
    // --- per-layer state (rewritten by the host each layer) ---
    float layerCount = 3.f;
    float currentLayer = 0.f;
    float layerRadius = 8.f;
    float gridSpacing = 8.f;        // round(radius * gridFactor)
    // --- Hertzmann core ---
    float threshold = 50.f;         // Lab error, OpenCV-encoded scale
    float maxStrokeLength = 16.f;
    float minStrokeLength = 4.f;
    float curvature = 1.f;
    // --- placement / compositing ---
    float gridFactor = 1.f;
    float opacity = 0.9f;
    float tensorSigma = 0.f;        // > 0 swaps in the structure-tensor field
    float firstLayer = 1.f;         // coarsest layer ignores the threshold
    // --- colour jitter ---
    float jitterHue = 0.05f;
    float jitterSat = 0.10f;
    float jitterVal = 0.10f;
    float sizeJitter = 0.f;
    // --- stroke variation / brush texture ---
    float angleJitter = 0.f;
    float opacityJitter = 0.f;
    float texStrength = 0.45f;      // web `brushTexture`
    float texRows = 16.f;           // used rows of this layer's tile
    // --- impasto ---
    float texTaper = 0.4f;          // web `textureTaper`
    float impastoStrength = 0.f;
    float impastoLight = 0.f;
    float lightAngle = 45.f;
    // --- misc ---
    float dryBrush = 0.f;
    float seedBudget = 200000.f;
    float frame = 0.f;
    float texLayerBase = 0.f;       // radius index * brush::VARIANTS
    // --- derived per layer ---
    float blurSigma = 4.f;          // radius * 0.5
    float chunk = 0.f;
    float chunkCount = 1.f;
    float texUScale = 1.f;          // TILE_W / max(16, radius * 4)
    // --- temporal ---
    float frameDiffThreshold = 0.f; // 0 = off; web scale is 0..255
    // --- relaxation (Hertzmann 2001) ---
    // areaWeight is w_area in E = E_app + w_area * sum(area). It is the knob
    // that decides economy vs fidelity; 0 makes relaxation a pure fit.
    float relaxAreaWeight = 0.06f;
    float relaxMoveScale = 0.8f;    // perturbation amplitude, in stroke radii
    float relaxIter = 0.f;          // current iteration, set by the pipeline
    float relaxCandidates = 6.f;    // trial moves per stroke per iteration
    float relaxRemove = 0.f;        // > 0 enables removal; 1.0 = break even
    // 0 folds the frame counter into every stroke's hash, so a stroke's
    // jitter and brush tile are redrawn each frame. That is what worker.js
    // does (it uses Math.random()), and on video it guarantees the strokes
    // boil even where the picture is identical. 1 hashes from position only,
    // which makes a still reproducible and a static region stable.
    float jitterPerFrame = 0.f;
    // Edge Tangent Flow neighbourhood radius, in pixels. Kang 2007 uses 5.
    // The iteration count is host-side (RenderConfig::etfIterations) because
    // it drives a loop of dispatches rather than anything inside one.
    float etfRadius = 5.f;
};
static_assert(sizeof(TuningParams) % 16 == 0, "Params must stay vec4-aligned");

// Host-side settings that are not part of the uniform block.
enum class Underpaint { Blur, None, Average };

struct RenderConfig {
    // Brush radii, coarse to fine. Web default is "8, 4, 2".
    std::vector<float> radii{8.f, 4.f, 2.f};
    Underpaint underpaint = Underpaint::Blur;
    float bristleDensity = 10.f;
    // Painting passes per layer. worker.js paints one stroke at a time and
    // each stroke sees every earlier stroke's paint, which is what stops
    // strokes early -- `makeCurvedStroke` breaks once the canvas already
    // matches the reference better than the stroke's own colour. With a single
    // pass per layer nothing in that layer has been painted yet when the
    // strokes trace, the break never fires, and every stroke runs to its full
    // length. Splitting the layer into passes restores the feedback: a stroke
    // in pass k sees the paint from passes 0..k-1. See system_architecture.md.
    int passesPerLayer = 8;
    // Relaxation iterations over the whole frame's strokes, after the greedy
    // pass has placed them. 0 = the plain Hertzmann 1998 result.
    int relaxIterations = 0;
    // Edge Tangent Flow refinements of the per-layer direction field (Kang
    // 2007). 0 leaves the Sobel or structure-tensor field exactly as it was,
    // which is the default: this is the one stage with no counterpart in the
    // web renderer, so it must be opt-in for the port to stay verifiable
    // against it. The paper converges in 2-3 passes.
    int etfIterations = 0;
    // Pyramidal Lucas-Kanade levels for temporal advection, above the quarter
    // resolution the flow is stored at. 0 disables the flow entirely, which
    // leaves temporal mode exactly as the web version has it: paint carried
    // forward in place, repainted wherever the pixel changed. Each extra level
    // doubles the largest motion that can be tracked, so 4 covers about 2^5 =
    // 32 px of displacement per frame.
    int flowLevels = 0;
    // LK refinements per pyramid level. Three is where it stops moving.
    int flowIterations = 3;
    // Hash-scattered subsets per relaxation iteration. Only useful now as a
    // way to stagger the work: the kernel scores against the source and the
    // frame's ground, so concurrent strokes no longer interfere and 1 is fine.
    int relaxSubPasses = 1;
};
