#include "test_framework.h"

#include "mcp/McpSculpt.h"

#include <cstring>
#include <vector>

// Suites for the sculpt kernel (#105): falloff curves, weld-seam integrity,
// two-phase smoothing, empty-selection no-op — all GL-free. MeshTopology is
// hand-built here (MeshTopology::Build lives in MeshEdit.cpp, which pulls GL
// through Mesh construction in its other functions).

namespace forge::test {
namespace {

Vertex V(vec3 p, vec3 n = {0.0f, 1.0f, 0.0f})
{
    return {p, n, {0.0f, 0.0f}};
}

// Quad split into two triangles with a DUPLICATED seam (A and C exist twice,
// as a UV seam would leave them): raw verts 0=A 1=B 2=C | 3=A' 4=C' 5=D.
struct SeamQuad {
    std::vector<Vertex> verts;
    MeshTopology topo;
};

SeamQuad BuildSeamQuad()
{
    SeamQuad q;
    const vec3 A{0.0f, 0.0f, 0.0f}, B{1.0f, 0.0f, 0.0f}, C{1.0f, 0.0f, 1.0f}, D{0.0f, 0.0f, 1.0f};
    q.verts = {V(A), V(B), V(C), V(A), V(C), V(D)};
    q.topo.weldGroup = {0, 1, 2, 0, 2, 3};
    q.topo.groups = {{0, 3}, {1}, {2, 4}, {5}};
    q.topo.groupNeighbors = {{1, 2, 3}, {0, 2}, {0, 1, 3}, {0, 2}};
    return q;
}

void FalloffCurves()
{
    CHECK(ApproxEq(FalloffWeight(SculptFalloff::Smooth, 0.0f), 1.0f, 1e-6f));
    CHECK(ApproxEq(FalloffWeight(SculptFalloff::Smooth, 0.5f), 0.5f, 1e-6f)); // 1-(0.75-0.25)
    CHECK(FalloffWeight(SculptFalloff::Smooth, 1.0f) == 0.0f);
    CHECK(FalloffWeight(SculptFalloff::Smooth, 0.25f) > FalloffWeight(SculptFalloff::Smooth, 0.75f));

    CHECK(ApproxEq(FalloffWeight(SculptFalloff::Linear, 0.25f), 0.75f, 1e-6f));
    CHECK(FalloffWeight(SculptFalloff::Linear, 1.0f) == 0.0f);

    CHECK(FalloffWeight(SculptFalloff::Constant, 0.0f) == 1.0f);
    CHECK(FalloffWeight(SculptFalloff::Constant, 0.999f) == 1.0f);
    CHECK(FalloffWeight(SculptFalloff::Constant, 1.0f) == 0.0f); // hard edge AT the radius

    // Beyond the radius everything is zero, negative t clamps to center weight.
    CHECK(FalloffWeight(SculptFalloff::Smooth, 2.0f) == 0.0f);
    CHECK(FalloffWeight(SculptFalloff::Linear, 5.0f) == 0.0f);
    CHECK(FalloffWeight(SculptFalloff::Smooth, -1.0f) == 1.0f);
}

void MoveKeepsSeamsWelded()
{
    SeamQuad q = BuildSeamQuad();
    // Brush covers only corner A (B is at distance 1 = 2x the radius).
    const size_t moved = SculptMove(q.verts, q.topo, {0.0f, 0.0f, 0.0f}, 0.5f,
                                    {0.0f, 1.0f, 0.0f}, SculptFalloff::Constant);
    CHECK(moved == 1);
    // Both raw copies of A moved identically — the seam did not tear.
    CHECK(ApproxEq(q.verts[0].position.y, 1.0f, 1e-6f));
    CHECK(ApproxEq(q.verts[3].position.y, 1.0f, 1e-6f));
    CHECK(q.verts[0].position == q.verts[3].position);
    // Everything else untouched.
    CHECK(q.verts[1].position.y == 0.0f && q.verts[2].position.y == 0.0f &&
          q.verts[5].position.y == 0.0f);
}

void MoveAppliesFalloffWeight()
{
    SeamQuad q = BuildSeamQuad();
    // Linear falloff, radius 2, centered on A: B sits at distance 1 -> w = 0.5.
    const size_t moved = SculptMove(q.verts, q.topo, {0.0f, 0.0f, 0.0f}, 2.0f,
                                    {0.0f, 0.0f, 2.0f}, SculptFalloff::Linear);
    CHECK(moved == 4); // every group is inside radius 2
    CHECK(ApproxEq(q.verts[0].position.z, 2.0f, 1e-5f)); // A: w = 1
    CHECK(ApproxEq(q.verts[1].position.z, 1.0f, 1e-5f)); // B: w = 0.5, moved 0 -> 1
}

void EmptySelectionIsNoOp()
{
    SeamQuad q = BuildSeamQuad();
    const std::vector<Vertex> before = q.verts;
    CHECK(SculptMove(q.verts, q.topo, {100.0f, 0.0f, 0.0f}, 1.0f, {0.0f, 1.0f, 0.0f},
                     SculptFalloff::Smooth) == 0);
    CHECK(SculptInflate(q.verts, q.topo, {100.0f, 0.0f, 0.0f}, 1.0f, 0.5f) == 0);
    CHECK(SculptSmooth(q.verts, q.topo, {100.0f, 0.0f, 0.0f}, 1.0f, 1.0f) == 0);
    CHECK(std::memcmp(before.data(), q.verts.data(), before.size() * sizeof(Vertex)) == 0);
}

void InflateFollowsNormalsAndSign()
{
    SeamQuad q = BuildSeamQuad(); // all normals +y
    size_t moved = SculptInflate(q.verts, q.topo, {0.0f, 0.0f, 0.0f}, 0.5f, 0.2f);
    CHECK(moved == 1);
    CHECK(ApproxEq(q.verts[0].position.y, 0.2f, 1e-6f)); // w = 1 at the center
    CHECK(ApproxEq(q.verts[3].position.y, 0.2f, 1e-6f)); // seam copy too

    // Negative amount dents inward.
    SeamQuad d = BuildSeamQuad();
    moved = SculptInflate(d.verts, d.topo, {0.0f, 0.0f, 0.0f}, 0.5f, -0.2f);
    CHECK(moved == 1);
    CHECK(ApproxEq(d.verts[0].position.y, -0.2f, 1e-6f));
}

void SmoothIsTwoPhase()
{
    // Plus-shape: a spike at y=1 whose only neighbors are four base verts, and
    // each base vert's only neighbor is the spike.
    std::vector<Vertex> verts = {V({0.0f, 1.0f, 0.0f}), V({1.0f, 0.0f, 0.0f}),
                                 V({-1.0f, 0.0f, 0.0f}), V({0.0f, 0.0f, 1.0f}),
                                 V({0.0f, 0.0f, -1.0f})};
    MeshTopology topo;
    topo.weldGroup = {0, 1, 2, 3, 4};
    topo.groups = {{0}, {1}, {2}, {3}, {4}};
    topo.groupNeighbors = {{1, 2, 3, 4}, {0}, {0}, {0}, {0}};

    const size_t moved = SculptSmooth(verts, topo, {0.0f, 1.0f, 0.0f}, 3.0f, 1.0f);
    CHECK(moved == 5);
    // Spike (w = 1): fully relaxed onto its neighbor average, the base plane.
    CHECK(ApproxEq(verts[0].position.y, 0.0f, 1e-5f));
    // Base verts must have been pulled toward the spike's PRE-move y=1. If the
    // kernel applied in place, the spike (processed first) would already sit
    // at y=0 and the base verts would not rise at all.
    CHECK(verts[1].position.y > 0.1f);
    CHECK(verts[4].position.y > 0.1f);
}

void MirroredMoveDoesNotDouble()
{
    // Both centers hit corner A of the seam quad. A summing implementation
    // would move A by 2x; the merged kernel writes each group once and the
    // mirrored side wins — interactive X-mirror's last-write-wins.
    SeamQuad q = BuildSeamQuad();
    // Differing offsets pin the winner: y == 2 proves the MIRRORED side wins
    // on overlap (last-write-wins); a sum would give 3, primary-wins 1.
    const size_t moved = SculptMoveMirrored(q.verts, q.topo, {0.0f, 0.0f, 0.0f},
                                            {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f},
                                            {0.0f, 2.0f, 0.0f}, 0.5f, SculptFalloff::Constant);
    CHECK(moved == 1); // distinct groups, not per-application counts
    CHECK(ApproxEq(q.verts[0].position.y, 2.0f, 1e-6f)); // mirrored offset, once
    CHECK(ApproxEq(q.verts[3].position.y, 2.0f, 1e-6f)); // seam copy welded

    // Disjoint regions: both applied, targets from pre-move positions.
    SeamQuad d = BuildSeamQuad();
    const size_t moved2 = SculptMoveMirrored(d.verts, d.topo, {0.0f, 0.0f, 0.0f},
                                             {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 1.0f},
                                             {0.0f, 2.0f, 0.0f}, 0.5f, SculptFalloff::Constant);
    CHECK(moved2 == 2);
    CHECK(ApproxEq(d.verts[0].position.y, 1.0f, 1e-6f)); // A got offsetA
    CHECK(ApproxEq(d.verts[2].position.y, 2.0f, 1e-6f)); // C got offsetB

    // One side missing entirely: only the reachable region moves.
    SeamQuad m = BuildSeamQuad();
    const size_t moved3 = SculptMoveMirrored(m.verts, m.topo, {0.0f, 0.0f, 0.0f},
                                             {0.0f, 1.0f, 0.0f}, {50.0f, 0.0f, 0.0f},
                                             {0.0f, 2.0f, 0.0f}, 0.5f, SculptFalloff::Constant);
    CHECK(moved3 == 1);
    CHECK(ApproxEq(m.verts[0].position.y, 1.0f, 1e-6f));
}

void SnapRespectsSideFilter()
{
    // Shift the quad fully onto the positive side (x in [1,2]): a snap
    // restricted to the negative side must fail and leave the point untouched.
    SeamQuad q = BuildSeamQuad();
    for (Vertex& v : q.verts)
        v.position.x += 1.0f;
    vec3 p{-0.5f, 0.0f, 0.0f};
    CHECK(!SnapToNearestVertex(q.verts, q.topo, p, -1));
    CHECK(p == vec3(-0.5f, 0.0f, 0.0f));
    CHECK(SnapToNearestVertex(q.verts, q.topo, p, +1));
    CHECK(p == vec3(1.0f, 0.0f, 0.0f)); // shifted corner A

    // On-plane verts (x == 0) qualify for BOTH sides: a center that mirrors
    // onto the plane resolves to the same vertex, which the handler then
    // collapses into a single application.
    SeamQuad o = BuildSeamQuad(); // corner A sits exactly at x = 0
    vec3 n{-0.1f, 0.0f, 0.1f};
    CHECK(SnapToNearestVertex(o.verts, o.topo, n, -1));
    CHECK(n == vec3(0.0f, 0.0f, 0.0f));
}

void SnapFindsNearestVertex()
{
    SeamQuad q = BuildSeamQuad();
    // A point hovering off the surface snaps to the closest representative.
    vec3 p{0.1f, 0.5f, -0.05f}; // nearest corner is A (0,0,0)
    CHECK(SnapToNearestVertex(q.verts, q.topo, p));
    CHECK(p == vec3(0.0f, 0.0f, 0.0f));

    p = {1.1f, -0.2f, 1.05f}; // nearest is C (1,0,1)
    CHECK(SnapToNearestVertex(q.verts, q.topo, p));
    CHECK(p == vec3(1.0f, 0.0f, 1.0f));

    // No valid groups -> false, point untouched.
    MeshTopology empty;
    vec3 keep{5.0f, 5.0f, 5.0f};
    CHECK(!SnapToNearestVertex(q.verts, empty, keep));
    CHECK(keep == vec3(5.0f, 5.0f, 5.0f));
}

void InvertedNormalDetector()
{
    // Quad in the y=0 plane with +y normals; interior reference below it.
    SeamQuad sane = BuildSeamQuad();
    const vec3 interior{0.5f, -1.0f, 0.5f}; // "inside" is under the sheet
    CHECK(InvertedNormalFraction(sane.verts, sane.topo, {0.5f, 0.0f, 0.5f}, 2.0f, interior) ==
          0.0f);

    // Flip every normal: now the whole region reads folded.
    SeamQuad folded = BuildSeamQuad();
    for (Vertex& v : folded.verts)
        v.normal = {0.0f, -1.0f, 0.0f};
    CHECK(InvertedNormalFraction(folded.verts, folded.topo, {0.5f, 0.0f, 0.5f}, 2.0f, interior) ==
          1.0f);

    // Half flipped (groups A and B of 4) -> fraction 0.5.
    SeamQuad half = BuildSeamQuad();
    half.verts[0].normal = half.verts[3].normal = {0.0f, -1.0f, 0.0f}; // group A (both copies)
    half.verts[1].normal = {0.0f, -1.0f, 0.0f};                       // group B
    CHECK(ApproxEq(
        InvertedNormalFraction(half.verts, half.topo, {0.5f, 0.0f, 0.5f}, 2.0f, interior), 0.5f,
        1e-6f));

    // Empty region -> 0, not NaN.
    CHECK(InvertedNormalFraction(sane.verts, sane.topo, {50.0f, 0.0f, 0.0f}, 1.0f, interior) ==
          0.0f);
}

void StaleTopologyIsSkippedNotFatal()
{
    std::vector<Vertex> verts = {V({0.0f, 0.0f, 0.0f})};
    MeshTopology topo;
    topo.weldGroup = {0};
    topo.groups = {{99}, {0, 42}}; // representative past the array; partial group
    topo.groupNeighbors = {{}, {0}};

    // Group 0 is skipped (bad representative); group 1's valid vert moves,
    // the out-of-range copy is ignored.
    const size_t moved = SculptMove(verts, topo, {0.0f, 0.0f, 0.0f}, 1.0f, {0.0f, 1.0f, 0.0f},
                                    SculptFalloff::Constant);
    CHECK(moved == 1);
    CHECK(ApproxEq(verts[0].position.y, 1.0f, 1e-6f));
    // Smooth finds no valid neighbor representative -> leaves the vert alone.
    SculptSmooth(verts, topo, {0.0f, 1.0f, 0.0f}, 1.0f, 1.0f);
    CHECK(ApproxEq(verts[0].position.y, 1.0f, 1e-6f));
}

} // namespace

void RunMcpSculptTests()
{
    FalloffCurves();
    MoveKeepsSeamsWelded();
    MoveAppliesFalloffWeight();
    EmptySelectionIsNoOp();
    InflateFollowsNormalsAndSign();
    SmoothIsTwoPhase();
    MirroredMoveDoesNotDouble();
    SnapRespectsSideFilter();
    SnapFindsNearestVertex();
    InvertedNormalDetector();
    StaleTopologyIsSkippedNotFatal();
    std::printf("[ok] mcp sculpt kernel tests done\n");
}

} // namespace forge::test
