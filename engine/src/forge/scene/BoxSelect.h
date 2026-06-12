#pragma once

#include "forge/core/Geometry.h"
#include "forge/core/Math.h"

#include <optional>

namespace forge {

// Screen-space rectangle in viewport UV ([0,1] both axes, y-down).
struct RectUV {
    vec2 min{0.0f};
    vec2 max{0.0f};
};

// Project a world-space AABB to the screen rect it covers. GL-free pure math
// (unit-tested headless): nullopt when the box is entirely behind the camera;
// conservative full-viewport rect when it straddles the near plane (perspective
// division is meaningless there — better to over-select than to drop an object
// the user clearly dragged across).
std::optional<RectUV> ProjectAABBToScreen(const mat4& viewProj, const AABB& worldBox);

inline bool RectsOverlap(const RectUV& a, const RectUV& b)
{
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y;
}

} // namespace forge
