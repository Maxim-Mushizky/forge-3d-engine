#pragma once

#include "forge/core/Math.h"

namespace forge {

// Transform snapping (#5): quantize editor edits to clean increments so a
// beginner can place objects flush without eyeballing sub-unit offsets. Pure
// math, no GL — so it is unit tested headless and shared by both edit paths:
// the gizmo (passes a snap vector to ImGuizmo) and the inspector drag fields.

// Round `value` to the nearest multiple of `step`. A non-positive step means
// "snapping off" and returns `value` unchanged — guarding here keeps a zero
// step from producing inf/NaN even though callers also gate on the toggle.
float SnapToStep(float value, float step);

// Component-wise SnapToStep with one uniform step. Drives translate (grid),
// scale (factor step), and rotation (pass Euler angles in degrees + an angle
// step) — every axis quantizes to the same increment.
vec3 SnapToStep(const vec3& value, float step);

} // namespace forge
