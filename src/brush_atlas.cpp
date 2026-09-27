#include "brush_atlas.h"

#include <algorithm>
#include <cmath>

namespace brush {
namespace {

// worker.js `mulberry32`. Ported exactly, including the int32 wraparound, so a
// given (radius index, variant) produces the same tile the web version does.
struct Mulberry32 {
    uint32_t s;
    explicit Mulberry32(uint32_t seed) : s(seed) {}
    float operator()() {
        s = s + 0x6D2B79F5u;
        uint32_t t = (s ^ (s >> 15)) * (1u | s);
        t = (t + ((t ^ (t >> 7)) * (61u | t))) ^ t;
        return float(double(t ^ (t >> 14)) / 4294967296.0);
    }
};

// brush-texture.js `makeBrushTile`.
void makeTile(float* out, int tw, int th, float density, Mulberry32& rand) {
    std::vector<float> profile(size_t(th), 1.0f);

    // Across-width bristle profile: full coverage minus seeded gaussian dips.
    const int nDips = std::max(2, int(std::lround(density * th / 24.0)));
    for (int k = 0; k < nDips; ++k) {
        const float c = rand() * th;
        const float dw = 0.6f + rand() * 1.6f;
        const float depth = 0.3f + rand() * 0.55f;
        for (int y = 0; y < th; ++y) {
            const float d = (y - c) / dw;
            profile[size_t(y)] -= depth * std::exp(-d * d);
        }
    }

    // Along-stroke value noise: cosine-interpolated control points, wrapped at
    // both ends so the tile repeats seamlessly along long strokes. Each row
    // gets its own phase so bristle streaks break up at different places.
    float ctrl[9];
    for (int i = 0; i < 8; ++i) ctrl[i] = rand();
    ctrl[8] = ctrl[0];
    std::vector<float> rowPhase(static_cast<size_t>(th), 0.0f);
    for (int y = 0; y < th; ++y) rowPhase[size_t(y)] = rand() * tw;

    for (int y = 0; y < th; ++y) {
        const float vAbs = std::fabs(th > 1 ? (float(y) / (th - 1)) * 2.0f - 1.0f : 0.0f);
        const float edge = 1.0f + 0.22f * std::max(0.0f, (vAbs - 0.6f) / 0.4f);
        const float p = std::min(1.15f, std::max(0.04f, profile[size_t(y)])) * edge;
        for (int x = 0; x < tw; ++x) {
            const float fx = (std::fmod(float(x) + rowPhase[size_t(y)], float(tw)) / tw) * 8.0f;
            const int i0 = int(std::floor(fx));
            const float f = fx - i0;
            const float cf = (1.0f - std::cos(f * 3.14159265358979f)) * 0.5f;
            const float n = ctrl[i0] * (1.0f - cf) + ctrl[i0 + 1] * cf;
            out[size_t(y) * tw + x] = std::min(1.2f, std::max(0.0f, p * (0.78f + 0.44f * n)));
        }
    }
}

} // namespace

int rowsForRadius(float r) {
    return std::max(8, std::min(TILE_ROWS_MAX,
                                int(std::lround(2.0f * std::max(1.0f, r)))));
}

TileArray makeTiles(const std::vector<float>& radii, float bristleDensity) {
    const float density = bristleDensity > 0.f ? bristleDensity : 10.f;

    TileArray out;
    out.layers = int(radii.size()) * VARIANTS;
    out.data.assign(size_t(TILE_W) * TILE_ROWS_MAX * size_t(out.layers), 0.f);
    out.rows.resize(radii.size());

    std::vector<float> scratch(size_t(TILE_W) * TILE_ROWS_MAX);
    for (size_t ri = 0; ri < radii.size(); ++ri) {
        const int th = rowsForRadius(radii[ri]);
        out.rows[ri] = th;
        for (int v = 0; v < VARIANTS; ++v) {
            // Same seed expression as makeBrushTextures.
            Mulberry32 rand(0x9E3779B9u ^ uint32_t(int(ri) * 131 + v * 7919));
            makeTile(scratch.data(), TILE_W, th, density, rand);

            // Copy into the layer's first `th` rows; the shader maps v into
            // that range via TEX_ROWS, so the unused rows are never sampled.
            float* dst = out.data.data() + size_t(ri * VARIANTS + v) * TILE_W * TILE_ROWS_MAX;
            std::copy(scratch.begin(), scratch.begin() + size_t(th) * TILE_W, dst);
        }
    }
    return out;
}

} // namespace brush
