#pragma once
#include <algorithm>
#include <array>
#include <cstddef>

namespace wallpaper
{

// The four corners of a layer card, in triangle-strip order.
//
// Winding matters because the rasterizer's front-face rule is one global
// setting (counter-clockwise) shared with the indexed meshes the .mdl importer
// produces and with the icosphere generator, which winds CCW from outside.  A
// card that turns the other way is classified back-facing, so a material
// asking to cull back faces loses the entire layer instead of saving fill.
// Seen from +Z, where the camera sits, the first strip triangle turns
// counter-clockwise.
struct CardVertex {
    std::array<float, 3> position;
    std::array<float, 2> texcoord;
};

// u0/v0 belong to the left/top corner, u1/v1 to the right/bottom one — the
// texture's V axis runs opposite to the card's Y axis.
inline std::array<CardVertex, 4> CardStrip(float left, float right, float bottom, float top,
                                           float u0, float u1, float v0, float v1, float z = 0.0f) {
    return { {
        { { left, top, z }, { u0, v0 } },
        { { left, bottom, z }, { u0, v1 } },
        { { right, top, z }, { u1, v0 } },
        { { right, bottom, z }, { u1, v1 } },
    } };
}

// SceneVertexArray takes one tightly packed array per attribute, so the strip
// has to be split back out into parallel position and texcoord arrays.
struct CardStripArrays {
    std::array<float, 12> position;
    std::array<float, 8>  texcoord;
};

inline CardStripArrays FlattenCardStrip(const std::array<CardVertex, 4>& card) {
    CardStripArrays out {};
    for (std::size_t i = 0; i < card.size(); i++) {
        std::copy(card[i].position.begin(), card[i].position.end(), out.position.begin() + (i * 3));
        std::copy(card[i].texcoord.begin(), card[i].texcoord.end(), out.texcoord.begin() + (i * 2));
    }
    return out;
}

// Twice the signed area of the strip's first triangle, projected on XY.
// Positive is counter-clockwise seen from +Z, i.e. front-facing.
inline float CardStripFacing(const std::array<CardVertex, 4>& v) {
    const float e1x = v[1].position[0] - v[0].position[0];
    const float e1y = v[1].position[1] - v[0].position[1];
    const float e2x = v[2].position[0] - v[0].position[0];
    const float e2y = v[2].position[1] - v[0].position[1];
    return e1x * e2y - e1y * e2x;
}

} // namespace wallpaper
