#pragma once

#include "forge/renderer/Mesh.h"

#include <cstdint>
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

} // namespace forge
