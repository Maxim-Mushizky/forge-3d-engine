#include "Spatial.h"

#include <cfloat>
#include <cmath>

namespace forge {

AABB TransformAABB(const AABB& box, const mat4& transform)
{
    AABB out;
    if (!box.Valid())
        return out; // never launder FLT_MAX sentinels into a "valid" universe box
    for (int i = 0; i < 8; ++i) {
        const vec3 corner{i & 1 ? box.max.x : box.min.x,
                          i & 2 ? box.max.y : box.min.y,
                          i & 4 ? box.max.z : box.min.z};
        out.Expand(vec3(transform * vec4(corner, 1.0f)));
    }
    return out;
}

OverlapResult OverlapAABB(const AABB& a, const AABB& b)
{
    OverlapResult r;
    if (!a.Valid() || !b.Valid()) {
        r.distance = FLT_MAX;
        return r;
    }

    // Per-axis signed overlap; negative = gap of that size on that axis.
    const vec3 overlap = glm::min(a.max, b.max) - glm::max(a.min, b.min);

    // An axis penetrates when the intersection has width — or when it is
    // zero-width but the shared coordinate sits strictly inside one of the
    // boxes: a flat box (ground plane) piercing a solid one also produces a
    // zero, and must not read as the face contact of two solid boxes.
    bool penetratesAll = true;
    for (int i = 0; i < 3 && penetratesAll; ++i) {
        if (overlap[i] > 0.0f)
            continue;
        if (overlap[i] < 0.0f) {
            penetratesAll = false; // real gap
            continue;
        }
        const float c = a.min[i] > b.min[i] ? a.min[i] : b.min[i];
        const bool insideA = a.min[i] < c && c < a.max[i];
        const bool insideB = b.min[i] < c && c < b.max[i];
        if (!insideA && !insideB)
            penetratesAll = false; // resting contact on this axis
    }
    if (!penetratesAll) {
        const vec3 gap = glm::max(-overlap, vec3(0.0f));
        r.distance = glm::length(gap); // 0 for pure face contact
        return r;
    }

    r.overlap = true;
    // MTV: per axis the cheaper of pushing A past B's max or past B's min.
    // NOT the raw overlap extent — when B contains A the extent is just A's
    // own size, and translating by it would leave the boxes still overlapping.
    int axis = 0;
    float best = FLT_MAX;
    float bestSigned = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float pushPos = b.max[i] - a.min[i]; // move A toward +axis to clear B
        const float pushNeg = a.max[i] - b.min[i]; // move A toward -axis to clear B
        const float d = pushPos < pushNeg ? pushPos : -pushNeg;
        if (std::fabs(d) < best) {
            best = std::fabs(d);
            bestSigned = d;
            axis = i;
        }
    }
    r.depth = best;
    r.penetration = vec3(0.0f);
    r.penetration[axis] = bestSigned;
    return r;
}

float DistanceToAABB(const AABB& box, const vec3& p)
{
    if (!box.Valid())
        return FLT_MAX;
    const vec3 d = glm::max(glm::max(box.min - p, p - box.max), vec3(0.0f));
    return glm::length(d);
}

float DistanceBetweenAABB(const AABB& a, const AABB& b)
{
    if (!a.Valid() || !b.Valid())
        return FLT_MAX;
    const vec3 gap = glm::max(glm::max(a.min - b.max, b.min - a.max), vec3(0.0f));
    return glm::length(gap);
}

std::optional<vec3> AabbLandmark(const AABB& box, const std::string& feature)
{
    if (!box.Valid())
        return std::nullopt;
    const vec3 c = (box.min + box.max) * 0.5f;
    if (feature == "center")
        return c;
    if (feature == "top")
        return vec3(c.x, box.max.y, c.z);
    if (feature == "bottom")
        return vec3(c.x, box.min.y, c.z);
    return std::nullopt;
}

std::optional<float> AabbAxisExtent(const AABB& box, const std::string& axis)
{
    if (!box.Valid())
        return std::nullopt;
    if (axis == "x")
        return box.max.x - box.min.x;
    if (axis == "y")
        return box.max.y - box.min.y;
    if (axis == "z")
        return box.max.z - box.min.z;
    return std::nullopt;
}

} // namespace forge
