#include "test_framework.h"

#include "forge/geometry/MeshStats.h"

#include <vector>

// Topology diagnostics kernel (#76): degenerate detection, boundary/non-manifold
// edges, watertightness with position welding (UV seams duplicate vertices).

namespace forge::test {
namespace {

Vertex V(float x, float y, float z, float u = 0.0f, float v = 0.0f)
{
    Vertex vert{};
    vert.position = {x, y, z};
    vert.uv = {u, v};
    return vert;
}

// Closed tetrahedron: 4 verts, 4 faces, consistently wound.
void TestTetrahedronIsWatertight()
{
    std::vector<Vertex> verts = {V(0, 0, 0), V(1, 0, 0), V(0, 1, 0), V(0, 0, 1)};
    std::vector<uint32_t> idx = {0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3};
    MeshStats s = ComputeMeshStats(verts, idx);
    CHECK(s.vertexCount == 4);
    CHECK(s.triangleCount == 4);
    CHECK(s.degenerateTriangles == 0);
    CHECK(s.boundaryEdges == 0);
    CHECK(s.nonManifoldEdges == 0);
    CHECK(s.watertight);
    CHECK(!s.hasUVs);
    CHECK(ApproxEq(s.bounds.min.x, 0.0f) && ApproxEq(s.bounds.max.x, 1.0f));
}

void TestSingleTriangleIsOpen()
{
    std::vector<Vertex> verts = {V(0, 0, 0), V(1, 0, 0), V(0, 1, 0)};
    std::vector<uint32_t> idx = {0, 1, 2};
    MeshStats s = ComputeMeshStats(verts, idx);
    CHECK(s.triangleCount == 1);
    CHECK(s.boundaryEdges == 3);
    CHECK(!s.watertight);
}

void TestDegenerateTriangleFlagged()
{
    // Second triangle collapses two corners onto the same position.
    std::vector<Vertex> verts = {V(0, 0, 0), V(1, 0, 0), V(0, 1, 0), V(1, 0, 0)};
    std::vector<uint32_t> idx = {0, 1, 2, 0, 1, 3}; // verts 1 and 3 coincide
    MeshStats s = ComputeMeshStats(verts, idx);
    CHECK(s.degenerateTriangles == 1);
}

void TestSeamDuplicatedVertsWeldClosed()
{
    // Tetrahedron with one corner duplicated (as a UV seam would); the weld
    // must still see a closed surface.
    std::vector<Vertex> verts = {V(0, 0, 0), V(1, 0, 0), V(0, 1, 0), V(0, 0, 1),
                                 V(0, 0, 1, /*u=*/0.5f, /*v=*/0.5f)}; // dup of vert 3
    std::vector<uint32_t> idx = {0, 2, 1, 0, 1, 3, 0, 4, 2, 1, 2, 4};
    MeshStats s = ComputeMeshStats(verts, idx);
    CHECK(s.boundaryEdges == 0);
    CHECK(s.watertight);
    CHECK(s.hasUVs); // the seam vertex carries a nonzero UV
}

void TestNonManifoldEdgeDetected()
{
    // Three triangles fanning off one shared edge (0-1).
    std::vector<Vertex> verts = {V(0, 0, 0), V(1, 0, 0), V(0, 1, 0), V(0, 0, 1), V(0, -1, 0)};
    std::vector<uint32_t> idx = {0, 1, 2, 0, 1, 3, 0, 1, 4};
    MeshStats s = ComputeMeshStats(verts, idx);
    CHECK(s.nonManifoldEdges == 1);
    CHECK(!s.watertight);
}

void TestEmptyMesh()
{
    MeshStats s = ComputeMeshStats({}, {});
    CHECK(s.vertexCount == 0);
    CHECK(s.triangleCount == 0);
    CHECK(!s.watertight); // an empty mesh encloses nothing
}

} // namespace

void RunMeshStatsTests()
{
    TestTetrahedronIsWatertight();
    TestSingleTriangleIsOpen();
    TestDegenerateTriangleFlagged();
    TestSeamDuplicatedVertsWeldClosed();
    TestNonManifoldEdgeDetected();
    TestEmptyMesh();
}

} // namespace forge::test
