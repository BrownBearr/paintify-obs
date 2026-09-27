#version 460 core

#include "common.glsl"

layout(std430, binding = 2) restrict readonly buffer VertexBuf { PackedVertex verts[]; };
layout(std430, binding = 4) restrict readonly buffer HeaderBuf { StrokeHeader headers[]; };

out vec3  vColor;
out float vNorm;     // signed across-width offset, -1..1 at the ribbon edges
out float vArcPx;    // arc length in px; < 0 or > totalLen inside the caps
flat out float vREff;
flat out float vRadius;
flat out float vTotalLen;
flat out float vOpacity;
flat out uint  vTexHash;

// One triangle-strip *instance* per stroke -- not one per segment. The old
// renderer drew each segment as its own quad, so every join blended twice and
// overlapping segments on a curve compounded; that double-compositing is why
// strokes looked like translucent wire instead of one opaque mark. worker.js
// unions all segments into a single mask and composites it once, and one
// instance per stroke is the GPU equivalent: a fragment is covered once.
//
// Two vertices per polyline point. Points past the stroke's real length
// collapse onto the last one, which emits zero-area triangles.

vec2 vpos(uint stroke, uint i) { return unpackHalf2x16(verts[stroke * MAX_VERTS + i].x); }
vec2 vrad(uint stroke, uint i) { return unpackHalf2x16(verts[stroke * MAX_VERTS + i].y); }

// brush-texture.js `taperLUT`: both tips shrink toward a floor set by
// textureTaper, with a smoothstep ramp over the first/last endFrac of arc.
float taperAt(float u, float amt) {
    if (amt <= 0.0) return 1.0;
    float endFrac = 0.06 + 0.30 * amt;
    float e = min(u, 1.0 - u);
    float s = clamp(e / endFrac, 0.0, 1.0);
    s = s * s * (3.0 - 2.0 * s);
    return (1.0 - 0.45 * amt) + 0.45 * amt * s;
}

void main() {
    uint stroke = uint(gl_InstanceID);
    StrokeHeader h = headers[stroke];

    uint vc = h.vertexCount;
    if (vc < 2u) {                       // worker.js: pts.length < 2 draws nothing
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
        vColor = vec3(0.0); vNorm = 0.0; vArcPx = 0.0;
        vREff = 0.0; vRadius = 0.0; vTotalLen = 1.0; vOpacity = 0.0; vTexHash = 0u;
        return;
    }

    uint i = min(uint(gl_VertexID) >> 1, vc - 1u);
    float side = (uint(gl_VertexID) & 1u) == 0u ? -1.0 : 1.0;

    vec2 p  = vpos(stroke, i);
    vec2 ra = vrad(stroke, i);
    float radius = ra.x;
    float arc = ra.y;

    // Tangent: segment direction at the ends, angle bisector in between.
    vec2 tIn  = (i > 0u)        ? normalize(p - vpos(stroke, i - 1u)) : vec2(0.0);
    vec2 tOut = (i + 1u < vc)   ? normalize(vpos(stroke, i + 1u) - p) : vec2(0.0);
    vec2 tang = (i == 0u) ? tOut : (i + 1u == vc ? tIn : normalize(tIn + tOut));
    if (dot(tang, tang) < 1e-8) tang = vec2(1.0, 0.0);

    vec2 nrm = vec2(-tang.y, tang.x);

    float u = arc / max(h.totalLen, 1e-6);
    float taper = (TEX_STRENGTH > 0.0) ? taperAt(u, clamp(TEX_TAPER, 0.0, 1.0)) : 1.0;
    float rEff = radius * taper;

    // Miter: lengthen the bisector so the two ribbon edges actually meet at a
    // bend. Clamped, or a hairpin turn would throw a spike across the canvas.
    float miter = 1.0;
    if (i > 0u && i + 1u < vc) {
        vec2 nIn = vec2(-tIn.y, tIn.x);
        miter = clamp(1.0 / max(abs(dot(nrm, nIn)), 0.25), 1.0, 4.0);
    }

    // Push the two outermost points out by a radius so the round caps have
    // geometry to live in; vArcPx then runs negative (or past totalLen) there
    // and the fragment shader turns that overshoot into the cap's curvature.
    float cap = 0.0;
    if (i == 0u)          cap = -rEff;
    else if (i + 1u == vc) cap =  rEff;

    vec2 pos = p + nrm * (rEff * miter * side) + tang * cap;

    // No y flip. Window y == 0 is the framebuffer's bottom row, which is
    // texel row 0 of the attached canvas texture, so negating y here would
    // store the canvas vertically mirrored relative to every compute stage --
    // and error.comp and trace.comp both sample the canvas by texel. The
    // readback and the on-screen blit do the flip to display orientation.
    gl_Position = vec4(pos.x / IMG_SIZE.x * 2.0 - 1.0,
                       pos.y / IMG_SIZE.y * 2.0 - 1.0, 0.0, 1.0);

    vec4 col = unpackUnorm4x8(h.color);
    vColor    = col.rgb;
    vOpacity  = col.a;
    vNorm     = side;
    vArcPx    = arc + cap;
    vREff     = rEff;
    vRadius   = radius;
    vTotalLen = h.totalLen;
    vTexHash  = h.layerTex >> 16;
}
