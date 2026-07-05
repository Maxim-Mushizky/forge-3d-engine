#include "test_framework.h"

#include <forge/geometry/Spatial.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cfloat>
#include <vector>

// Suites for the spatial introspection kernel (#94): world-AABB transforms
// under rotation, overlap/penetration math, and distance queries — all GL-free.

namespace forge::test {

static AABB BoxAt(const vec3& center, const vec3& halfExtents)
{
    AABB b;
    b.Expand(center - halfExtents);
    b.Expand(center + halfExtents);
    return b;
}

static void TransformRotation()
{
    // Unit cube spun 45° about Y: the xz footprint widens to the diagonal
    // (sqrt(2)), height is untouched. The naive min/max-only transform would
    // report the original extents — this is the case that breaks it.
    const AABB unit = BoxAt(vec3(0.0f), vec3(0.5f));
    const mat4 rot = glm::rotate(mat4(1.0f), glm::radians(45.0f), vec3(0.0f, 1.0f, 0.0f));
    const AABB w = TransformAABB(unit, rot);
    const float halfDiag = std::sqrt(2.0f) * 0.5f;
    CHECK(ApproxEq(w.min.x, -halfDiag));
    CHECK(ApproxEq(w.max.x, halfDiag));
    CHECK(ApproxEq(w.min.z, -halfDiag));
    CHECK(ApproxEq(w.max.z, halfDiag));
    CHECK(ApproxEq(w.min.y, -0.5f));
    CHECK(ApproxEq(w.max.y, 0.5f));
}

static void TransformTRS()
{
    const AABB unit = BoxAt(vec3(0.0f), vec3(0.5f));
    mat4 m = glm::translate(mat4(1.0f), vec3(10.0f, 2.0f, -3.0f));
    m = glm::scale(m, vec3(2.0f, 4.0f, 1.0f));
    const AABB w = TransformAABB(unit, m);
    CHECK(ApproxEq(w.min.x, 9.0f) && ApproxEq(w.max.x, 11.0f));
    CHECK(ApproxEq(w.min.y, 0.0f) && ApproxEq(w.max.y, 4.0f));
    CHECK(ApproxEq(w.min.z, -3.5f) && ApproxEq(w.max.z, -2.5f));

    // Invalid input box must stay invalid, not become a universe box.
    CHECK(!TransformAABB(AABB{}, m).Valid());
}

static void OverlapSeparatedAndTouching()
{
    const AABB a = BoxAt(vec3(0.0f), vec3(0.5f));

    // Clear gap along x only.
    OverlapResult r = OverlapAABB(a, BoxAt({3.0f, 0.0f, 0.0f}, vec3(0.5f)));
    CHECK(!r.overlap);
    CHECK(ApproxEq(r.distance, 2.0f));

    // Diagonal gap: 1 on x and 1 on y -> sqrt(2) between closest corners.
    r = OverlapAABB(a, BoxAt({2.0f, 2.0f, 0.0f}, vec3(0.5f)));
    CHECK(!r.overlap);
    CHECK(ApproxEq(r.distance, std::sqrt(2.0f)));

    // Exact face contact: legitimate stacking, not interpenetration.
    r = OverlapAABB(a, BoxAt({1.0f, 0.0f, 0.0f}, vec3(0.5f)));
    CHECK(!r.overlap);
    CHECK(ApproxEq(r.distance, 0.0f));

    // Invalid boxes never overlap and report an infinite gap.
    r = OverlapAABB(a, AABB{});
    CHECK(!r.overlap);
    CHECK(r.distance == FLT_MAX);
}

static void OverlapPenetration()
{
    const AABB a = BoxAt(vec3(0.0f), vec3(0.5f));

    // B centered at x=0.8: 0.2 deep on x, full overlap on y/z -> MTV is -x
    // (push A away from B, whose center is to the right).
    OverlapResult r = OverlapAABB(a, BoxAt({0.8f, 0.0f, 0.0f}, vec3(0.5f)));
    CHECK(r.overlap);
    CHECK(ApproxEq(r.depth, 0.2f));
    CHECK(ApproxEq(r.penetration.x, -0.2f));
    CHECK(ApproxEq(r.penetration.y, 0.0f) && ApproxEq(r.penetration.z, 0.0f));

    // Symmetric: A on the other side pushes +x.
    r = OverlapAABB(BoxAt({0.8f, 0.0f, 0.0f}, vec3(0.5f)), a);
    CHECK(r.overlap);
    CHECK(ApproxEq(r.penetration.x, 0.2f));

    // Least-penetration axis wins: deep on x, shallow on y -> MTV along y.
    r = OverlapAABB(a, BoxAt({0.1f, 0.9f, 0.0f}, vec3(0.5f)));
    CHECK(r.overlap);
    CHECK(ApproxEq(r.depth, 0.1f));
    CHECK(ApproxEq(r.penetration.x, 0.0f));
    CHECK(ApproxEq(r.penetration.y, -0.1f));

    // Containment: the raw overlap extent is just the small box's own size
    // (0.5) and moving by it would NOT separate — the true escape is pushing
    // its trailing face past big.max: 2.0 - 1.25 = 0.75.
    const AABB big = BoxAt(vec3(0.0f), vec3(2.0f));
    const AABB small = BoxAt({1.5f, 0.0f, 0.0f}, {0.25f, 0.25f, 0.25f});
    r = OverlapAABB(small, big);
    CHECK(r.overlap);
    CHECK(ApproxEq(r.depth, 0.75f));
    CHECK(ApproxEq(r.penetration.x, 0.75f));
    // And the MTV really does separate: translate and re-test.
    AABB moved = small;
    moved.min += r.penetration;
    moved.max += r.penetration;
    CHECK(!OverlapAABB(moved, big).overlap);

    // The acceptance case: two unit spheres 1.0 apart interpenetrate by 1.0
    // (their unit AABBs overlap 1.0 on x).
    r = OverlapAABB(BoxAt(vec3(0.0f), vec3(1.0f)), BoxAt({1.0f, 0.0f, 0.0f}, vec3(1.0f)));
    CHECK(r.overlap);
    CHECK(ApproxEq(r.depth, 1.0f));
}

static void PointAndBoxDistance()
{
    const AABB box = BoxAt(vec3(0.0f), vec3(1.0f));
    CHECK(ApproxEq(DistanceToAABB(box, {0.5f, -0.5f, 0.0f}), 0.0f)); // inside
    CHECK(ApproxEq(DistanceToAABB(box, {3.0f, 0.0f, 0.0f}), 2.0f)); // face
    CHECK(ApproxEq(DistanceToAABB(box, {2.0f, 2.0f, 2.0f}), std::sqrt(3.0f))); // corner
    CHECK(DistanceToAABB(AABB{}, vec3(0.0f)) == FLT_MAX);

    CHECK(ApproxEq(DistanceBetweenAABB(box, BoxAt({4.0f, 0.0f, 0.0f}, vec3(1.0f))), 2.0f));
    CHECK(ApproxEq(DistanceBetweenAABB(box, BoxAt({0.5f, 0.0f, 0.0f}, vec3(1.0f))), 0.0f));
    CHECK(DistanceBetweenAABB(box, AABB{}) == FLT_MAX);
}

static void RadiusQuerySemantics()
{
    // Kernel-level mirror of the 20-entity acceptance test: unit boxes strung
    // along x at 0,2,4,...,38; radius 5 from the origin reaches the boxes at
    // x=0,2,4 (surface distances 0,1,3) and excludes x=6 (distance 5... its
    // near face is at 5.5). Exact set, not "roughly nearby".
    std::vector<AABB> boxes;
    for (int i = 0; i < 20; ++i)
        boxes.push_back(BoxAt({2.0f * (float)i, 0.0f, 0.0f}, vec3(0.5f)));

    int within = 0;
    for (const AABB& b : boxes)
        if (DistanceToAABB(b, vec3(0.0f)) <= 5.0f)
            ++within;
    CHECK(within == 3);

    CHECK(ApproxEq(DistanceToAABB(boxes[3], vec3(0.0f)), 5.5f)); // first excluded
}

void RunSpatialTests()
{
    TransformRotation();
    TransformTRS();
    OverlapSeparatedAndTouching();
    OverlapPenetration();
    PointAndBoxDistance();
    RadiusQuerySemantics();
    std::printf("[ok] spatial kernel tests done\n");
}

} // namespace forge::test
