#pragma once

#include "forge/core/Math.h"

namespace forge {

// Maps the user-facing subsurface parameters (surface color + per-channel mean
// free path) onto the volumetric coefficients the path tracer's random walk
// consumes. Lives CPU-side so the mapping is packed once per material at
// upload instead of re-derived per hit, and so the formulas are unit-testable
// without GL (tests/test_sss.cpp pins the inversion round trip).

// Radii below this floor (world units) are clamped: the walk enters the
// surface through a ~2e-3 ray offset, so shorter mean free paths could not
// find their way back to the wall within the step cap and would render black
// instead of converging to plain diffuse. At/below this scale the two are
// visually identical anyway — use subsurface = 0 for true opaque.
inline constexpr float kSssMinRadius = 5e-3f;

// Single-scattering albedo whose multi-scattered reflectance on a
// semi-infinite slab equals `color` (van de Hulst inversion). Per channel.
vec3 SssSingleScatterAlbedo(const vec3& color);

// Extinction coefficient sigma_t per channel: the reciprocal of the mean free
// path, floored at kSssMinRadius.
vec3 SssExtinction(const vec3& radius);

} // namespace forge
