#pragma once

#include <forge/core/Geometry.h>
#include <forge/core/Math.h>

#include <string>
#include <vector>

namespace forge {

// Camera recipes for the render_views tool (#93). GL-free: pure glm math over
// the target's world AABB, so the presets are unit-testable without a context.
struct ViewSpec {
    vec3 eye{0.0f};
    vec3 center{0.0f};
    vec3 up{0.0f, 1.0f, 0.0f};
    bool ortho = false;
    float orthoHalf = 1.0f; // half-extent of the ortho box (world units)
    float fovDeg = 40.0f;
    std::string label;
};

// Presets: "turntable" (front/right/back/left orbit), "4up" (front/right/
// top-ortho/three-quarter), "top_ortho" (single top-down plan view).
// Unknown preset -> empty vector.
std::vector<ViewSpec> BuildViewSpecs(const std::string& preset, const AABB& target);

mat4 ViewProjFor(const ViewSpec& spec, float aspect);

// Golden-ratio hue walk: stable, visually distinct entity colors for the
// object-id diagnostic mode.
vec3 IdColor(size_t index);

// World-space AABB of a local-space box under an affine transform (all 8
// corners, not just min/max — rotations would break the shortcut).
AABB TransformAABB(const AABB& box, const mat4& transform);

} // namespace forge
