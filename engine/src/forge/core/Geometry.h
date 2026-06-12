#pragma once

#include "forge/core/Math.h"

#include <algorithm>
#include <cfloat>

namespace forge {

struct Ray {
    vec3 origin{0.0f};
    vec3 direction{0.0f, 0.0f, -1.0f};
};

struct AABB {
    vec3 min{FLT_MAX};
    vec3 max{-FLT_MAX};

    void Expand(const vec3& p)
    {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    bool Valid() const { return min.x <= max.x; }
};

// Slab test. Division by zero direction components yields IEEE infinities, which
// the min/max logic handles correctly.
inline bool RayIntersectsAABB(const Ray& ray, const AABB& box, float& tOut)
{
    vec3 invD = 1.0f / ray.direction;
    vec3 t0 = (box.min - ray.origin) * invD;
    vec3 t1 = (box.max - ray.origin) * invD;
    vec3 tNear = glm::min(t0, t1);
    vec3 tFar = glm::max(t0, t1);
    float tMin = std::max({tNear.x, tNear.y, tNear.z});
    float tMax = std::min({tFar.x, tFar.y, tFar.z});
    if (tMax < 0.0f || tMin > tMax)
        return false;
    tOut = tMin > 0.0f ? tMin : tMax;
    return true;
}

// Möller–Trumbore.
inline bool RayIntersectsTriangle(const Ray& ray, const vec3& v0, const vec3& v1, const vec3& v2, float& tOut)
{
    constexpr float kEpsilon = 1e-7f;
    vec3 e1 = v1 - v0;
    vec3 e2 = v2 - v0;
    vec3 p = glm::cross(ray.direction, e2);
    float det = glm::dot(e1, p);
    if (std::abs(det) < kEpsilon)
        return false;
    float invDet = 1.0f / det;
    vec3 tvec = ray.origin - v0;
    float u = glm::dot(tvec, p) * invDet;
    if (u < 0.0f || u > 1.0f)
        return false;
    vec3 q = glm::cross(tvec, e1);
    float v = glm::dot(ray.direction, q) * invDet;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    float t = glm::dot(e2, q) * invDet;
    if (t <= kEpsilon)
        return false;
    tOut = t;
    return true;
}

} // namespace forge
