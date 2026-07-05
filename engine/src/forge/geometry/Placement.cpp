#include "Placement.h"

#include "forge/geometry/Spatial.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace forge {

static vec3 Center(const AABB& b) { return (b.min + b.max) * 0.5f; }

vec3 SolveOn(const AABB& entity, const AABB& anchor, float clearance)
{
    if (!entity.Valid() || !anchor.Valid())
        return vec3(0.0f);
    vec3 delta(0.0f);
    delta.y = anchor.max.y + clearance - entity.min.y;
    const vec3 c = Center(entity);
    const bool insideFootprint = c.x >= anchor.min.x && c.x <= anchor.max.x &&
                                 c.z >= anchor.min.z && c.z <= anchor.max.z;
    if (!insideFootprint) {
        const vec3 a = Center(anchor);
        delta.x = a.x - c.x;
        delta.z = a.z - c.z;
    }
    return delta;
}

int NearestSide(const AABB& entity, const AABB& anchor)
{
    const vec3 off = Center(entity) - Center(anchor);
    if (std::fabs(off.x) >= std::fabs(off.z))
        return off.x >= 0.0f ? 0 : 1;
    return off.z >= 0.0f ? 2 : 3;
}

vec3 SolveAgainst(const AABB& entity, const AABB& anchor, int side, float clearance)
{
    if (!entity.Valid() || !anchor.Valid())
        return vec3(0.0f);
    const vec3 e = Center(entity);
    const vec3 a = Center(anchor);
    vec3 delta(0.0f);
    delta.y = anchor.min.y - entity.min.y; // stand on the anchor's floor
    switch (side) {
    case 0: // +x face
        delta.x = anchor.max.x + clearance - entity.min.x;
        delta.z = a.z - e.z;
        break;
    case 1: // -x face
        delta.x = anchor.min.x - clearance - entity.max.x;
        delta.z = a.z - e.z;
        break;
    case 2: // +z face
        delta.z = anchor.max.z + clearance - entity.min.z;
        delta.x = a.x - e.x;
        break;
    default: // -z face
        delta.z = anchor.min.z - clearance - entity.max.z;
        delta.x = a.x - e.x;
        break;
    }
    return delta;
}

float YawToward(const vec3& from, const vec3& to)
{
    // Local +z is "forward" (yaw 0 faces +z); atan2(dx, dz) rotates it onto
    // the target direction about +y. Degenerate (same XZ) -> keep yaw 0.
    const float dx = to.x - from.x;
    const float dz = to.z - from.z;
    if (dx == 0.0f && dz == 0.0f)
        return 0.0f;
    return std::atan2(dx, dz);
}

std::vector<PlacedPose> SolveAround(const AABB& entity, const AABB& anchor, int count,
                                    float clearance)
{
    std::vector<PlacedPose> poses;
    if (count <= 0 || !entity.Valid() || !anchor.Valid())
        return poses;

    const vec3 a = Center(anchor);
    const vec3 e = Center(entity);
    const vec3 anchorExt = anchor.max - anchor.min;
    const vec3 entityExt = entity.max - entity.min;
    const float anchorHalfDiag = 0.5f * std::hypot(anchorExt.x, anchorExt.z);
    const float entityHalfDiag = 0.5f * std::hypot(entityExt.x, entityExt.z);
    const float radius = anchorHalfDiag + clearance + entityHalfDiag;

    poses.reserve((size_t)count);
    for (int k = 0; k < count; ++k) {
        // Slot 0 in front of the anchor (-z side), walking counter-clockwise.
        const float angle = glm::radians(180.0f) + glm::two_pi<float>() * (float)k / (float)count;
        vec3 target = a;
        target.x += std::sin(angle) * radius;
        target.z += std::cos(angle) * radius;
        PlacedPose p;
        p.delta = vec3(target.x - e.x, anchor.min.y - entity.min.y, target.z - e.z);
        p.yawRad = YawToward(target, a);
        poses.push_back(p);
    }
    return poses;
}

vec3 NudgeOut(const AABB& moving, const std::vector<AABB>& obstacles, int maxIterations)
{
    if (!moving.Valid())
        return vec3(0.0f);
    AABB box = moving;
    vec3 total(0.0f);
    for (int it = 0; it < maxIterations; ++it) {
        bool clean = true;
        for (const AABB& o : obstacles) {
            const OverlapResult r = OverlapAABB(box, o);
            if (!r.overlap)
                continue;
            box.min += r.penetration;
            box.max += r.penetration;
            total += r.penetration;
            clean = false;
        }
        if (clean)
            break;
    }
    return total;
}

static float Feature(const AABB& b, int axis, AlignMode mode)
{
    switch (mode) {
    case AlignMode::Min: return b.min[axis];
    case AlignMode::Max: return b.max[axis];
    default: return (b.min[axis] + b.max[axis]) * 0.5f;
    }
}

std::vector<float> SolveAlign(const std::vector<AABB>& boxes, int axis, AlignMode mode,
                              float target)
{
    std::vector<float> deltas(boxes.size(), 0.0f);
    for (size_t i = 0; i < boxes.size(); ++i)
        if (boxes[i].Valid())
            deltas[i] = target - Feature(boxes[i], axis, mode);
    return deltas;
}

std::vector<float> SolveDistribute(const std::vector<AABB>& boxes, int axis, float spacing)
{
    std::vector<float> deltas(boxes.size(), 0.0f);
    std::vector<size_t> order;
    for (size_t i = 0; i < boxes.size(); ++i)
        if (boxes[i].Valid())
            order.push_back(i);
    if (order.size() < 2)
        return deltas;
    std::sort(order.begin(), order.end(), [&](size_t l, size_t r) {
        return Feature(boxes[l], axis, AlignMode::Center) <
               Feature(boxes[r], axis, AlignMode::Center);
    });

    if (spacing >= 0.0f) {
        // Pack: fixed gap between neighbouring faces, first box anchored.
        float cursor = boxes[order[0]].max[axis];
        for (size_t i = 1; i < order.size(); ++i) {
            const AABB& b = boxes[order[i]];
            deltas[order[i]] = cursor + spacing - b.min[axis];
            cursor = b.max[axis] + deltas[order[i]];
        }
        return deltas;
    }

    // Spread: even centre spacing between the current first and last centres.
    const float first = Feature(boxes[order.front()], axis, AlignMode::Center);
    const float last = Feature(boxes[order.back()], axis, AlignMode::Center);
    const float step = (last - first) / (float)(order.size() - 1);
    for (size_t i = 1; i + 1 < order.size(); ++i)
        deltas[order[i]] = first + step * (float)i -
                           Feature(boxes[order[i]], axis, AlignMode::Center);
    return deltas;
}

} // namespace forge
