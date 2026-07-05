#pragma once

#include "forge/core/Geometry.h"
#include "forge/core/Math.h"

namespace forge {

// GL-free spatial reasoning kernel (#94): world-AABB transforms, overlap tests,
// and point/box distance — the symbolic backbone for the MCP spatial tools, so
// an agent can detect interpenetration and proximity without reading pixels.

// World-space AABB of a local-space box under an affine transform (all 8
// corners, not just min/max — rotations would break the shortcut).
AABB TransformAABB(const AABB& box, const mat4& transform);

struct OverlapResult {
    bool overlap = false;
    vec3 penetration{0.0f}; // MTV: translate A by this to just separate it from B
    float depth = 0.0f;     // |penetration| (smallest-axis overlap)
    float distance = 0.0f;  // gap between the boxes when not overlapping, else 0
};

// AABB-vs-AABB. Face contact (zero-width intersection on some axis) counts as
// NOT overlapping — stacked/abutting placements are legitimate, only actual
// interpenetration should alarm an agent. Invalid boxes never overlap.
OverlapResult OverlapAABB(const AABB& a, const AABB& b);

// Distance from a point to the box surface; 0 inside. FLT_MAX for invalid
// boxes so radius queries naturally exclude them.
float DistanceToAABB(const AABB& box, const vec3& p);

// Distance between two boxes (closest points); 0 when touching/overlapping.
float DistanceBetweenAABB(const AABB& a, const AABB& b);

} // namespace forge
