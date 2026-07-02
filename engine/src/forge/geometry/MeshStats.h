#pragma once

#include "forge/core/Geometry.h"
#include "forge/renderer/Mesh.h"

#include <cstdint>
#include <vector>

namespace forge {

// Topology diagnostics for the agent feedback loop (#76): cheap facts an LLM
// can act on without seeing the mesh. Watertightness is a closed-2-manifold
// edge test after welding coincident positions (UV seams duplicate vertices,
// which would otherwise make every primitive read as open).
struct MeshStats {
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t degenerateTriangles = 0; // zero-area or repeated-corner tris
    uint32_t boundaryEdges = 0;       // edges used by exactly one triangle
    uint32_t nonManifoldEdges = 0;    // edges used by three or more triangles
    bool watertight = false;          // closed 2-manifold after position weld
    bool hasUVs = false;              // any vertex carries a nonzero UV
    AABB bounds;                      // object space
};

// Pure kernel — no GL, no Mesh handle — so it unit-tests headless.
MeshStats ComputeMeshStats(const std::vector<Vertex>& vertices,
                           const std::vector<uint32_t>& indices);

} // namespace forge
