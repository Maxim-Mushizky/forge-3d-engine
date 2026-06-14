#pragma once

#include "forge/renderer/Mesh.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace forge {

// Editable vertex/edge/face view of a triangle mesh, for Edit Mode (#52). The
// render Mesh is triangle-soup with per-face-duplicated seam verts; EditMesh
// merges co-located verts into weld groups (the "real" vertices), exposes
// group-space edges and per-triangle faces, and classifies each edge so the
// overlay can hide triangulation diagonals across flat faces.
//
// Built in object space and GL-free on purpose: it re-derives weld groups from
// raw vertex/index vectors (the same 1e-4 quantization the rest of the engine
// uses) instead of going through Mesh, whose constructor needs a GL context.
// That keeps the whole module unit-testable headless.

enum class EdgeKind : uint8_t {
    Boundary,    // 1 incident face — an open border
    Manifold,    // 2 incident faces — the normal case, dihedral is meaningful
    NonManifold, // >2 incident faces — dihedral undefined
};

struct EditVertex {
    vec3 position;                  // object-space group representative
    std::vector<uint32_t> rawVerts; // raw mesh vertex indices welded into this group (bake-back, T2+)
    std::vector<uint32_t> edges;    // incident EditEdge ids
    std::vector<uint32_t> faces;    // incident EditFace ids
};

struct EditEdge {
    uint32_t v0 = 0, v1 = 0;     // EditVertex ids, v0 < v1
    std::vector<uint32_t> faces; // incident EditFace ids (size drives EdgeKind)
    EdgeKind kind = EdgeKind::Boundary;
    float dihedral = 0.0f;       // radians between the two incident face normals; 0 unless Manifold
};

struct EditFace {
    uint32_t v[3] = {0, 0, 0};            // EditVertex ids, source winding
    uint32_t edges[3] = {0, 0, 0};        // EditEdge ids opposite-free (UINT32_MAX if degenerate)
    vec3 centroid{0.0f};                  // object space
    vec3 normal{0.0f, 1.0f, 0.0f};        // normalized, cross(p1 - p0, p2 - p0)
};

struct EditMesh {
    std::vector<EditVertex> vertices;
    std::vector<EditEdge> edges;
    std::vector<EditFace> faces;
    std::unordered_map<uint64_t, uint32_t> edgeLookup; // (v0<<32 | v1) -> index into edges
};

// Sentinel for a face edge slot that collapsed (a degenerate triangle with two
// co-located corners contributes no edge there).
inline constexpr uint32_t kNoEdge = 0xFFFFFFFFu;

// Build from raw geometry — the testable core; never touches Mesh/GL.
EditMesh BuildEditMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
// Convenience for editor call sites; forwards to the raw-vector core.
EditMesh BuildEditMesh(const Mesh& mesh);

// Order-independent edge lookup; nullptr if a == b or the edge does not exist.
const EditEdge* FindEdge(const EditMesh& mesh, uint32_t a, uint32_t b);

// Whether the edge should be drawn as a wireframe line. Boundary and
// non-manifold edges are always real; a manifold edge counts only when its
// dihedral exceeds the threshold, which hides the diagonal splitting a flat
// quad face (its two triangles are coplanar, dihedral ~0).
bool IsCreaseEdge(const EditEdge& edge, float angleThresholdRad);

} // namespace forge
