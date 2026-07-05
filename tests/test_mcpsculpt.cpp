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
    StaleTopologyIsSkippedNotFatal();
    std::printf("[ok] mcp sculpt kernel tests done\n");
}

} // namespace forge::test
