// Shared declarations for every stage. gl_util.cpp splices this in wherever a
// shader writes `#include "common.glsl"` -- there is no GL_ARB_shading_language_include
// on every driver and the manual splice keeps line numbers honest via #line.
//
// This is a port of PainterlyImageCreatorWeb's `paintHertzmann` (worker.js) to
// the GPU. Where a name matches a JS identifier it means the same thing and is
// calibrated on the same scale -- in particular THRESHOLD is compared against
// an OpenCV-encoded Lab distance (L*255/100, a+128, b+128), so the web UI's
// default of 50 transfers over unchanged.

// The members carry an sbrP prefix on purpose: the accessors below are macros,
// so a plainly-named member (p0, p1, ...) would be captured by any local of the
// same name -- which silently turned IMG_SIZE into a polyline point once.
layout(std140, binding = 0) uniform Params {
    vec4 sbrP0;  // imgW, imgH, texelW, texelH
    vec4 sbrP1;  // layerCount, currentLayer, layerRadius, gridSpacing
    vec4 sbrP2;  // threshold, maxStrokeLength, minStrokeLength, curvature
    vec4 sbrP3;  // gridFactor, opacity, tensorSigma, firstLayer
    vec4 sbrP4;  // jitterHue, jitterSat, jitterVal, sizeJitter
    vec4 sbrP5;  // angleJitter, opacityJitter, texStrength, texRows
    vec4 sbrP6;  // texTaper, impastoStrength, impastoLight, lightAngle
    vec4 sbrP7;  // dryBrush, seedBudget, frame, texLayerBase
    vec4 sbrP8;  // blurSigma, chunk, chunkCount, texUScale
    vec4 sbrP9;  // frameDiffThreshold, relaxAreaWeight, relaxMoveScale, relaxIter
    vec4 sbrP10; // relaxCandidates, relaxRemove, jitterPerFrame, etfRadius
};

#define IMG_SIZE        sbrP0.xy
#define TEXEL           sbrP0.zw
#define LAYER_COUNT     sbrP1.x
#define CUR_LAYER       sbrP1.y
#define LAYER_RADIUS    sbrP1.z
#define GRID_SPACING    sbrP1.w
#define THRESHOLD       sbrP2.x
#define MAX_STROKE_LEN  sbrP2.y
#define MIN_STROKE_LEN  sbrP2.z
#define CURVATURE       sbrP2.w
#define GRID_FACTOR     sbrP3.x
#define OPACITY         sbrP3.y
#define TENSOR_SIGMA    sbrP3.z
#define FIRST_LAYER     sbrP3.w
#define JITTER_HUE      sbrP4.x
#define JITTER_SAT      sbrP4.y
#define JITTER_VAL      sbrP4.z
#define SIZE_JITTER     sbrP4.w
#define ANGLE_JITTER    sbrP5.x
#define OPACITY_JITTER  sbrP5.y
#define TEX_STRENGTH    sbrP5.z
#define TEX_ROWS        sbrP5.w
#define TEX_TAPER       sbrP6.x
#define IMPASTO_STR     sbrP6.y
#define IMPASTO_LIGHT   sbrP6.z
#define LIGHT_ANGLE     sbrP6.w
#define DRY_BRUSH       sbrP7.x
#define SEED_BUDGET     sbrP7.y
#define FRAME           sbrP7.z
#define TEX_LAYER_BASE  sbrP7.w
#define BLUR_SIGMA      sbrP8.x
#define CHUNK           sbrP8.y
#define CHUNK_COUNT     sbrP8.z
#define TEX_USCALE      sbrP8.w
#define FRAME_DIFF      sbrP9.x
#define RELAX_AREA_W    sbrP9.y
#define RELAX_MOVE      sbrP9.z
#define RELAX_ITER      sbrP9.w
#define RELAX_CANDS     sbrP10.x
#define RELAX_REMOVE    sbrP10.y
#define JITTER_PER_FRAME sbrP10.z
#define ETF_RADIUS      sbrP10.w

// Frame salt for the per-stroke hashes. Zero unless jitterPerFrame is on, so
// by default a stroke's jitter and brush tile depend on where it is and not on
// when it was painted -- a still renders identically every run, and a static
// region of a video keeps the same strokes instead of boiling.
uint frameSalt() { return (JITTER_PER_FRAME > 0.5) ? uint(FRAME) : 0u; }

// Vertices per polyline. Matches the web UI's max-stroke-length cap of 32,
// plus one for the seed point itself.
const uint MAX_VERTS = 33u;

// One seed = one stroke to be traced. 32 bytes, std430.
struct Seed {
    vec2  pos;
    vec2  dir;         // zero on a fresh seed: the first step picks the gradient
    uint  color;       // packUnorm4x8, .a = per-stroke opacity scale
    float radius;      // already size-jittered
    uint  layerTex;    // layer | (brush texture variant << 16)
    float pad;
};

// One traced polyline. 16 bytes, std430. Colour lives here rather than on
// every vertex: it is constant along a stroke, and .a carries the stroke's
// jittered opacity, so the rasteriser needs nothing else per stroke.
struct StrokeHeader {
    uint  vertexCount;
    uint  layerTex;
    uint  color;       // packUnorm4x8(rgb, opacity)
    float totalLen;    // arc length in px, for the brush tile's u coordinate
};

// Frame-wide stroke pool counters. Unlike CounterBuf these survive the whole
// frame: the greedy phase appends to the pool pass by pass, and relaxation then
// works over everything it collected.
struct PoolCounters {
    uint count;     // strokes in the pool
    uint base;      // slot the current pass reserved
    uint removed;   // strokes relaxation deleted
    uint accepted;  // perturbations relaxation kept
};

// Packed polyline vertex, 8 bytes. Colour is per-stroke and lives in the
// header, so it does not need to be repeated here.
//   x = packHalf2x16(pos)
//   y = packHalf2x16(vec2(radius, cumulative arc length))
#define PackedVertex uvec2

// --- hashing -------------------------------------------------------------
uint hashU(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16; return x;
}
float hash01(uint x) { return float(hashU(x) & 0x00ffffffu) / 16777216.0; }
vec2  hash2(uint x)  { return vec2(hash01(x), hash01(x ^ 0x9e3779b9u)); }

// Mirrors brush-texture.js `brushHash`: the tile variant must be picked from
// the seed position, never from a frame-varying counter, or video flickers.
uint brushHash(int x, int y, int layer) {
    uint h = uint(x * 374761393 + y * 668265263 + layer * 2246822519);
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// --- colour --------------------------------------------------------------
// Web uses the NTSC luma weights for the gradient's grayscale, and divides by
// 255 -- keep both so `gmag` lands on the same scale as `computeGradients`.
float grayNTSC(vec3 c255) { return dot(c255, vec3(0.299, 0.587, 0.114)) / 255.0; }

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

// sRGB [0..1] -> Lab, encoded like OpenCV uint8 (L*255/100, a+128, b+128).
// THRESHOLD and the error map are both on this scale; changing the encoding
// silently rescales the web UI's threshold slider.
vec3 rgb2labCV(vec3 c) {
    vec3 lin = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
    float X = dot(lin, vec3(0.4124564, 0.3575761, 0.1804375)) / 0.95047;
    float Y = dot(lin, vec3(0.2126729, 0.7151522, 0.0721750));
    float Z = dot(lin, vec3(0.0193339, 0.1191920, 0.9503041)) / 1.08883;
    vec3 t = vec3(X, Y, Z);
    vec3 f = mix(7.787 * t + 16.0 / 116.0, pow(max(t, 1e-8), vec3(1.0 / 3.0)),
                 step(vec3(0.008856), t));
    return vec3((116.0 * f.y - 16.0) * 2.55, 500.0 * (f.x - f.y) + 128.0,
                200.0 * (f.y - f.z) + 128.0);
}
