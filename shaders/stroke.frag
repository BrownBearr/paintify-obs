#version 460 core

#include "common.glsl"

// Brush tiles, one array layer per (radius index, variant). Generated on the
// host by a direct port of brush-texture.js `makeBrushTile`, so the bristle
// profile, the along-stroke value noise and the edge boost are the same
// content the web version modulates coverage with.
layout(binding = 4) uniform sampler2DArray uBrush;

in vec3  vColor;
in float vNorm;
in float vArcPx;
flat in float vREff;
flat in float vRadius;
flat in float vTotalLen;
flat in float vOpacity;
flat in uint  vTexHash;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out float fragHeight;

const float BRUSH_TW = 64.0;
const float BRUSH_TH = 48.0;   // allocated rows; TEX_ROWS of them are in use

void main() {
    // Coverage, matching worker.js `cov = rEff - dist + 0.5` clamped to 0..1.
    // Perpendicular distance is |vNorm| * vREff because the ribbon edges are
    // at |vNorm| == 1 by construction. Overshoot past either end turns the
    // flat tip into a round cap, which is what `drawCircle` does on the ends
    // of the web's mask.
    float perp = abs(vNorm) * vREff;
    float over = max(0.0, max(-vArcPx, vArcPx - vTotalLen));
    float dist = sqrt(perp * perp + over * over);
    float cov = clamp(vREff - dist + 0.5, 0.0, 1.0);
    if (cov <= 0.0) discard;

    // Brush texture: u tiles along arc length, v is the signed across-width
    // offset. uScale puts one tile repeat every max(16, radius*4) px, the same
    // rule renderStrokeSolid applies before it walks the segments.
    if (TEX_STRENGTH > 0.0) {
        float uOff  = float((vTexHash >> 3) & 63u);
        float layer = TEX_LAYER_BASE + float(vTexHash & 7u);
        float rows  = max(TEX_ROWS, 2.0);

        float tu = fract((vArcPx * TEX_USCALE + uOff) / BRUSH_TW);
        float row = clamp(vNorm * 0.5 + 0.5, 0.0, 1.0) * (rows - 1.0);
        float tv = (row + 0.5) / BRUSH_TH;

        float tile = texture(uBrush, vec3(tu, tv, layer)).r;
        cov *= (1.0 - TEX_STRENGTH) + TEX_STRENGTH * tile;
    }

    // Dry brush: worker.js fades opacity per segment along the stroke. The
    // ribbon has no segments to fade, so the same ramp is applied continuously
    // over arc length, which is the limit of that loop as segments shrink.
    float op = vOpacity;
    if (DRY_BRUSH > 0.0) {
        op *= max(0.0, 1.0 - DRY_BRUSH * clamp(vArcPx / max(vTotalLen, 1e-6), 0.0, 1.0));
    }

    float a = min(1.0, cov * op);
    if (a < 0.002 && IMPASTO_STR <= 0.0) discard;

    fragColor  = vec4(vColor * a, a);        // premultiplied
    // Height accumulates the *pre-opacity* coverage, additively, exactly as
    // `heightBuf[...] += mv * impastoStrength`.
    fragHeight = cov * IMPASTO_STR;
}
