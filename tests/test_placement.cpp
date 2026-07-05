#include "test_framework.h"

#include <forge/geometry/Placement.h>
#include <forge/geometry/Spatial.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <vector>

// Suites for the relational-placement solver (#95): on/against/around poses,
// yaw math, collision nudge, align/distribute — all GL-free.

namespace forge::test {

static AABB PBox(const vec3& center, const vec3& halfExtents)
{
    AABB b;
    b.Expand(center - halfExtents);
    b.Expand(center + halfExtents);
    return b;
}

static AABB Shifted(const AABB& b, const vec3& d) { return AABB{b.min + d, b.max + d}; }

static void OnRelation()
{
    const AABB desk = PBox({5.0f, 0.5f, 5.0f}, {1.0f, 0.5f, 0.6f});

    // Lamp far away: recentred over the desk, bottom exactly on the desktop.
    const AABB lamp = PBox({0.0f, 3.0f, 0.0f}, {0.15f, 0.3f, 0.15f});
    vec3 d = SolveOn(lamp, desk, 0.0f);
    AABB placed = Shifted(lamp, d);
    CHECK(ApproxEq(placed.min.y, 1.0f));
    CHECK(ApproxEq((placed.min.x + placed.max.x) * 0.5f, 5.0f));
    CHECK(ApproxEq((placed.min.z + placed.max.z) * 0.5f, 5.0f));
    CHECK(!OverlapAABB(placed, desk).overlap); // face contact, not interpenetration

    // Lamp already over a desk corner: keeps its XZ, only the height changes.
    const AABB corner = PBox({5.8f, 3.0f, 5.4f}, {0.15f, 0.3f, 0.15f});
    d = SolveOn(corner, desk, 0.0f);
    CHECK(ApproxEq(d.x, 0.0f) && ApproxEq(d.z, 0.0f));
    CHECK(ApproxEq(Shifted(corner, d).min.y, 1.0f));

    // Clearance hovers; invalid inputs are inert.
    d = SolveOn(lamp, desk, 0.25f);
    CHECK(ApproxEq(Shifted(lamp, d).min.y, 1.25f));
    CHECK(glm::length(SolveOn(AABB{}, desk, 0.0f)) == 0.0f);
    CHECK(glm::length(SolveOn(lamp, AABB{}, 0.0f)) == 0.0f);

    // Rotated anchor: the world AABB of a 45-degree desk is wider, and the
    // rest height is unchanged — placement runs on world boxes.
    const mat4 rot = glm::rotate(mat4(1.0f), glm::radians(45.0f), {0.0f, 1.0f, 0.0f});
    const AABB rotDesk = TransformAABB(PBox({0.0f, 0.5f, 0.0f}, {1.0f, 0.5f, 0.6f}), rot);
    d = SolveOn(lamp, rotDesk, 0.0f);
    CHECK(ApproxEq(Shifted(lamp, d).min.y, 1.0f));

    // Stacked anchors: resting on the top crate of a stack uses the top's max.
    const AABB topCrate = PBox({0.0f, 2.5f, 0.0f}, {0.5f, 0.5f, 0.5f});
    d = SolveOn(lamp, topCrate, 0.0f);
    CHECK(ApproxEq(Shifted(lamp, d).min.y, 3.0f));
}

static void AgainstRelation()
{
    const AABB wall = PBox({0.0f, 1.5f, 0.0f}, {5.0f, 1.5f, 0.25f});
    const AABB crate = PBox({2.0f, 3.0f, 4.0f}, {0.5f, 0.5f, 0.5f});

    CHECK(NearestSide(crate, wall) == 2); // crate sits toward +z
    CHECK(NearestSide(PBox({-4.0f, 0.0f, 1.0f}, vec3(0.5f)), wall) == 1);

    vec3 d = SolveAgainst(crate, wall, 2, 0.0f);
    AABB placed = Shifted(crate, d);
    CHECK(ApproxEq(placed.min.z, 0.25f));                          // abuts the +z face
    CHECK(ApproxEq((placed.min.x + placed.max.x) * 0.5f, 0.0f));   // centred on the run
    CHECK(ApproxEq(placed.min.y, 0.0f));                           // bottoms aligned
    CHECK(!OverlapAABB(placed, wall).overlap);

    d = SolveAgainst(crate, wall, 1, 0.1f); // -x side with a gap
    placed = Shifted(crate, d);
    CHECK(ApproxEq(placed.max.x, -5.1f));
    CHECK(ApproxEq(OverlapAABB(placed, wall).distance, 0.1f));
}

static void YawFacing()
{
    const vec3 o(0.0f);
    CHECK(ApproxEq(YawToward(o, {0.0f, 0.0f, 5.0f}), 0.0f));                    // +z ahead
    CHECK(ApproxEq(YawToward(o, {5.0f, 0.0f, 0.0f}), glm::radians(90.0f)));     // +x
    CHECK(ApproxEq(YawToward(o, {-5.0f, 0.0f, 0.0f}), glm::radians(-90.0f)));   // -x
    CHECK(ApproxEq(std::fabs(YawToward(o, {0.0f, 0.0f, -5.0f})), glm::radians(180.0f)));
    CHECK(ApproxEq(YawToward(o, {0.0f, 7.0f, 0.0f}), 0.0f)); // degenerate: same XZ
}

static void AroundRelation()
{
    const AABB table = PBox({2.0f, 0.4f, -3.0f}, {0.8f, 0.4f, 0.8f});
    const AABB chair = PBox({10.0f, 0.5f, 10.0f}, {0.25f, 0.5f, 0.25f});

    const auto poses = SolveAround(chair, table, 4, 0.1f);
    CHECK(poses.size() == 4);

    const float radius = 0.5f * std::hypot(1.6f, 1.6f) + 0.1f + 0.5f * std::hypot(0.5f, 0.5f);
    const vec3 tc{2.0f, 0.4f, -3.0f};
    for (size_t i = 0; i < poses.size(); ++i) {
        const AABB placed = Shifted(chair, poses[i].delta);
        const vec3 c = (placed.min + placed.max) * 0.5f;
        CHECK(ApproxEq(std::hypot(c.x - tc.x, c.z - tc.z), radius, 1e-3f)); // on the ring
        CHECK(ApproxEq(placed.min.y, 0.0f));                                // table's floor
        CHECK(!OverlapAABB(placed, table).overlap);
        // Every chair faces the table: yaw's forward (+z rotated about y)
        // points from the chair toward the table centre.
        const vec3 fwd{std::sin(poses[i].yawRad), 0.0f, std::cos(poses[i].yawRad)};
        const vec3 toTable = glm::normalize(vec3(tc.x - c.x, 0.0f, tc.z - c.z));
        CHECK(glm::dot(fwd, toTable) > 0.999f);
        // And none of the chairs overlap each other.
        for (size_t j = 0; j < i; ++j)
            CHECK(!OverlapAABB(placed, Shifted(chair, poses[j].delta)).overlap);
    }
    // Slot 0 sits in front (-z of the anchor).
    const AABB first = Shifted(chair, poses[0].delta);
    CHECK((first.min.z + first.max.z) * 0.5f < tc.z);

    CHECK(SolveAround(chair, table, 0, 0.0f).empty());
    CHECK(SolveAround(chair, AABB{}, 4, 0.0f).empty());
}

static void Nudging()
{
    const AABB box = PBox(vec3(0.0f), vec3(0.5f));

    // Single obstacle: pushed out along the least-penetration axis, clean after.
    std::vector<AABB> obstacles{PBox({0.8f, 0.0f, 0.0f}, vec3(0.5f))};
    vec3 n = NudgeOut(box, obstacles);
    CHECK(!OverlapAABB(Shifted(box, n), obstacles[0]).overlap);
    CHECK(ApproxEq(n.x, -0.2f));

    // Two obstacles biting on different axes: both MTV steps apply and the
    // box settles into the corner between them (face contact only).
    obstacles = {PBox({0.85f, 0.0f, 0.0f}, vec3(0.5f)),
                 PBox({-0.5f, 0.0f, 0.9f}, vec3(0.5f))};
    n = NudgeOut(box, obstacles);
    CHECK(ApproxEq(n.x, -0.15f) && ApproxEq(n.z, -0.1f));
    const AABB settled = Shifted(box, n);
    for (const AABB& o : obstacles)
        CHECK(!OverlapAABB(settled, o).overlap);

    // Unsolvable squeeze (corridor narrower than the box): NudgeOut gives up
    // after maxIterations instead of looping forever — the caller reports the
    // residual overlap.
    obstacles = {PBox({0.6f, 0.0f, 0.0f}, {0.5f, 5.0f, 5.0f}),
                 PBox({-0.9f, 0.0f, 0.0f}, {0.5f, 5.0f, 5.0f})};
    n = NudgeOut(box, obstacles, 8);
    CHECK(std::isfinite(n.x) && std::isfinite(n.y) && std::isfinite(n.z));

    CHECK(glm::length(NudgeOut(box, {})) == 0.0f); // nothing to avoid
}

static void AlignBoxes()
{
    const std::vector<AABB> boxes{PBox({0.0f, 1.0f, 0.0f}, vec3(1.0f)),
                                  PBox({5.0f, 3.0f, 2.0f}, vec3(0.5f)),
                                  PBox({-2.0f, 0.5f, 7.0f}, {0.5f, 0.25f, 0.5f})};

    // Align y centres to the first box's centre (1.0).
    auto d = SolveAlign(boxes, 1, AlignMode::Center, 1.0f);
    CHECK(ApproxEq(d[0], 0.0f) && ApproxEq(d[1], -2.0f) && ApproxEq(d[2], 0.5f));

    // Min faces to x = 10.
    d = SolveAlign(boxes, 0, AlignMode::Min, 10.0f);
    CHECK(ApproxEq(d[0], 11.0f) && ApproxEq(d[1], 5.5f) && ApproxEq(d[2], 12.5f));

    // Max faces to z = 0; invalid boxes stay put.
    std::vector<AABB> withInvalid{boxes[0], AABB{}};
    d = SolveAlign(withInvalid, 2, AlignMode::Max, 0.0f);
    CHECK(ApproxEq(d[0], -1.0f) && d[1] == 0.0f);
}

static void DistributeBoxes()
{
    // Deliberately out of order along x: centres at 6, 0, 3.
    const std::vector<AABB> boxes{PBox({6.0f, 0.0f, 0.0f}, vec3(0.5f)),
                                  PBox({0.0f, 0.0f, 0.0f}, vec3(0.5f)),
                                  PBox({3.0f, 0.0f, 0.0f}, vec3(1.0f))};

    // Pack with gap 0.5: order preserved (0, 3, 6), first box anchored.
    auto d = SolveDistribute(boxes, 0, 0.5f);
    CHECK(ApproxEq(d[1], 0.0f));  // leftmost box anchors the run
    CHECK(ApproxEq(d[2], -1.0f)); // middle: min 2.0 -> first's max 0.5 + gap = 1.0
    CHECK(ApproxEq(d[0], -2.0f)); // last: min 5.5 -> moved middle's max 3.0 + gap = 3.5

    // Even spread: first and last centres pinned, middle centred halfway.
    d = SolveDistribute(boxes, 0, -1.0f);
    CHECK(ApproxEq(d[1], 0.0f) && ApproxEq(d[0], 0.0f));
    CHECK(ApproxEq(d[2], 0.0f)); // centre 3 already the midpoint of 0 and 6

    const std::vector<AABB> uneven{PBox({0.0f, 0.0f, 0.0f}, vec3(0.5f)),
                                   PBox({1.0f, 0.0f, 0.0f}, vec3(0.5f)),
                                   PBox({6.0f, 0.0f, 0.0f}, vec3(0.5f))};
    d = SolveDistribute(uneven, 0, -1.0f);
    CHECK(ApproxEq(d[1], 2.0f)); // centre 1 -> 3

    // Fewer than two valid boxes: all zeros.
    d = SolveDistribute({PBox(vec3(0.0f), vec3(1.0f)), AABB{}}, 0, 0.5f);
    CHECK(d[0] == 0.0f && d[1] == 0.0f);
}

void RunPlacementTests()
{
    OnRelation();
    AgainstRelation();
    YawFacing();
    AroundRelation();
    Nudging();
    AlignBoxes();
    DistributeBoxes();
    std::printf("[ok] placement solver tests done\n");
}

} // namespace forge::test
