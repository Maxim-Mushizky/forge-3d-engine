#pragma once

#include "forge/renderer/Mesh.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace forge {

// Topology helpers for sculpting. Built once when entering sculpt mode.
// Meshes duplicate vertices along UV seams (sphere longitude seam, cube corners);
// sculpting raw indices would tear the mesh open — the weld map groups co-located
// vertices so they move and shade as one.
struct MeshTopology {
    // weldGroup[i] = group id of vertex i; groups[g] = vertex indices in group g
    std::vector<uint32_t> weldGroup;
    std::vector<std::vector<uint32_t>> groups;
    // neighbors of each weld GROUP (group ids), for Laplacian smoothing
    std::vector<std::vector<uint32_t>> groupNeighbors;

    static MeshTopology Build(const Mesh& mesh);
};

// Full normal recompute: face-normal accumulation, then averaged across weld
// groups so seams stay shading-continuous. ~1-2 ms at 100k verts.
void RecomputeNormalsWelded(Mesh& mesh, const MeshTopology& topology);

// Faceted shading: write each triangle's geometric normal to its three corners.
// A vertex shared by triangles with differing normals takes the last writer —
// exact on the crease-duplicated primitives (each face owns its corners),
// approximate on fully-welded surfaces (Shade Flat, #65 T6).
void RecomputeNormalsFlat(Mesh& mesh);

// Edge map in weld-GROUP space. Raw-index edges would split along UV seams
// (primitives are topologically open there) and report false boundaries;
// group-space edges see the surface as closed. tris holds triangle ids (i/3).
struct MeshEdgeMap {
    struct Edge {
        uint32_t g0, g1; // weld groups, g0 < g1
        std::vector<uint32_t> tris;
    };
    std::vector<Edge> edges;
    std::unordered_map<uint64_t, uint32_t> lookup; // (g0<<32 | g1) -> index into edges

    static MeshEdgeMap Build(const Mesh& mesh, const MeshTopology& topology);
    const Edge* Find(uint32_t groupA, uint32_t groupB) const;
};

// Bake an X-mirror across object-space x=0 into new geometry. Vertices within
// 1e-4 of the plane snap to x=0 and are index-shared (watertight seam, no
// epsilon merge); others are duplicated with x and normal.x negated. Mirrored
// triangles get reversed winding. Geometry already at x<0 is kept untouched
// (no clipping in v1 — caller may warn). Normals are recomputed welded.
std::shared_ptr<Mesh> MirrorBakeX(const Mesh& mesh);

// Whole-mesh uniform Laplacian smooth (#77): each weld group relaxes toward
// the average of its neighbor groups, `iterations` Jacobi passes blended by
// `strength` in [0,1]. Seam copies move together (group space), UVs untouched.
std::shared_ptr<Mesh> LaplacianSmoothMesh(const Mesh& mesh, float strength, int iterations);

// Loop subdivision: 4x triangle count. Positions are smoothed in weld-GROUP
// space (both seam copies read the same group-edge position — no cracks) while
// connectivity and UVs stay on raw indices (seams keep their own UVs).
// keepShape = plain midpoint split, no smoothing: adds sculpt resolution
// without moving the surface. Boundary edges use the boundary rules; non-
// manifold edges fall back to midpoints. Degenerate triangles are dropped.
std::shared_ptr<Mesh> LoopSubdivide(const Mesh& mesh, bool keepShape);

} // namespace forge
