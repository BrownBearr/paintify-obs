#pragma once
#include <cstdint>
#include <vector>

// Brush tiles, ported from PainterlyImageCreatorWeb's brush-texture.js. One
// tile set per brush radius (tile height tracks stroke thickness) times
// BRUSH_VARIANTS variants, uploaded as a single 2D array texture.
//
// Tile space: u = position along the stroke (tiles horizontally, seamless),
// v = signed perpendicular offset / radius mapped to tile rows. Content =
// across-width bristle profile x low-frequency along-stroke noise x edge
// boost near the border.
namespace brush {

constexpr int TILE_W = 64;        // BRUSH_TEX_W
constexpr int TILE_ROWS_MAX = 48; // allocated rows; `rows` of them are used
constexpr int VARIANTS = 8;       // BRUSH_TEX_VARIANTS

struct TileArray {
    std::vector<float> data;      // TILE_W * TILE_ROWS_MAX * layers, R16F-bound
    std::vector<int> rows;        // used row count per radius index
    int layers = 0;               // radii.size() * VARIANTS
};

// `radii` must be the per-layer brush radii, coarse to fine. Layer index for
// radius i, variant v is i * VARIANTS + v.
TileArray makeTiles(const std::vector<float>& radii, float bristleDensity);

// Row count the web would pick for a given radius, for TEX_ROWS.
int rowsForRadius(float r);

} // namespace brush
