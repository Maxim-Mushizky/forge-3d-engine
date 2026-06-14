#include "TransformSnap.h"

#include <cmath>

namespace forge {

float SnapToStep(float value, float step)
{
    if (step <= 0.0f)
        return value;
    return std::round(value / step) * step;
}

vec3 SnapToStep(const vec3& value, float step)
{
    return {SnapToStep(value.x, step), SnapToStep(value.y, step), SnapToStep(value.z, step)};
}

} // namespace forge
