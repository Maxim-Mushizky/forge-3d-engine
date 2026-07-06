#include "Sss.h"

#include <algorithm>
#include <cmath>

namespace forge {

// Algorithm choice (#112): random-walk SSS over a Burley diffusion BSSRDF.
// The walk reuses the existing closest-hit BVH trace in a bounded inner loop,
// handles thin/curved geometry without the planar assumption, and needs none
// of the probe-ray multi-hit + 3-axis x 3-channel MIS machinery diffusion
// sampling requires — the same trade Cycles, Hyperion and Arnold settled on
// (Chiang et al., "Practical and Controllable Subsurface Scattering for
// Production Path Tracing", SIGGRAPH 2016). The walk itself lives in
// pathtrace.comp; this file is the parameter mapping it is fed.

// Van de Hulst's semi-infinite-slab approximation gives the multi-scattered
// reflectance of a medium with single-scatter albedo a as
//   R(a) = (1 - s)(1 - 0.139 s) / (1 + 1.17 s),  s = sqrt(1 - a)
// This is its closed-form inverse (d'Eon, "A Hitchhiker's Guide to Multiple
// Scattering" eq. 53.7; the isotropic case of Cycles' random-walk remap), so
// a surface parameterized by `color` scatters out looking like `color`.
static float SingleScatterAlbedo(float color)
{
    const float a = std::clamp(color, 0.0f, 1.0f);
    const float s = 4.09712f + 4.20863f * a - std::sqrt(9.59217f + 41.6808f * a + 17.7126f * a * a);
    // 1 - s^2 lands exactly on [0,1] at the endpoints; the upper clamp keeps a
    // pure-white medium absorbing a little so walks terminate.
    return std::clamp(1.0f - s * s, 0.0f, 0.999999f);
}

vec3 SssSingleScatterAlbedo(const vec3& color)
{
    return {SingleScatterAlbedo(color.x), SingleScatterAlbedo(color.y),
            SingleScatterAlbedo(color.z)};
}

vec3 SssExtinction(const vec3& radius)
{
    return {1.0f / std::max(radius.x, kSssMinRadius), 1.0f / std::max(radius.y, kSssMinRadius),
            1.0f / std::max(radius.z, kSssMinRadius)};
}

} // namespace forge
