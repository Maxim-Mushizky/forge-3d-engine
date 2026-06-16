#pragma once

#include "forge/renderer/Mesh.h"

#include <cstdint>
#include <unordered_map>
#include <utility>
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

// Which element type a selection refers to (shared by the editor's edit mode).
enum class ElementKind : uint8_t { Vertex, Edge, Face };

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

// --- screen-space pick kernels (#54) -------------------------------------
// Pure 2D math on already-projected points (the editor handles the
// camera/world->screen projection, which needs GL state; these stay GL-free
// so element picking is unit tested headless). Coordinates are viewport
// pixels, but any consistent 2D space works.

// Shortest distance from point p to segment a-b. Degenerate (a == b) returns
// the distance to that point.
float DistancePointSegment2D(const vec2& p, const vec2& a, const vec2& b);

// Whether p lies inside triangle a-b-c (winding-independent; edges inclusive).
bool PointInTriangle2D(const vec2& p, const vec2& a, const vec2& b, const vec2& c);

// Whether p lies within the axis-aligned rect [mn, mx] (inclusive).
bool PointInRect2D(const vec2& p, const vec2& mn, const vec2& mx);

// --- element transform helpers (#59) -------------------------------------
// Pure, GL-free so they unit-test headless; the editor owns the gizmo.

// Deduplicated EditVertex ids that `selected` (ids of `kind` elements) touches:
// vertices as-is, an edge's two endpoints, or a face's three corners.
std::vector<uint32_t> ResolveVertexSet(const EditMesh& mesh, ElementKind kind,
                                       const std::vector<uint32_t>& selected);

// Mean object-space position of the given EditVertex ids (origin if empty).
vec3 SelectionCentroid(const EditMesh& mesh, const std::vector<uint32_t>& vertexIds);

// Bake a transform into raw mesh vertices: for each EditVertex id, map its
// startPositions entry by xform and write the result to ALL of that group's
// rawVerts (so welded seam copies move together). vertexIds and startPositions
// are parallel. Only positions are touched.
void ApplyVertexTransform(std::vector<Vertex>& meshVertices, const EditMesh& mesh,
                          const std::vector<uint32_t>& vertexIds, const std::vector<vec3>& startPositions,
                          const mat4& xform);

// --- face extrude (#63) ---------------------------------------------------
// Pure region->geometry math so it unit-tests headless; the editor owns the
// interactive drag and the undo command.

// New geometry for a face-extrude, built at zero offset (the cap coincides
// with the original surface). The editor slides `capVerts` along `normal`.
struct FaceExtrusion {
    std::vector<Vertex> vertices;    // source verts + duplicated cap + wall verts
    std::vector<uint32_t> indices;   // source tris (selection re-pointed to the cap) + walls
    std::vector<uint32_t> capVerts;  // indices into `vertices` that slide: cap + wall tops
    vec3 normal{0.0f, 1.0f, 0.0f};   // object-space slide direction (normalized)
};

// Build push-pull geometry for a face selection. The selected triangles become
// a duplicated cap (the original surface stays as a floor referenced only by the
// walls); each boundary edge of the region — an edge on exactly one selected
// face — grows a wall quad, while shared interior edges get none, so a connected
// region extrudes as a single cap. selectedFaces are EditFace ids (== triangle
// ids). Returns an empty result (no indices) if the selection is empty, refers
// to a stale id, or the area-weighted region normal is degenerate (e.g. opposite
// faces that cancel).
FaceExtrusion BuildFaceExtrusion(const EditMesh& mesh, const std::vector<Vertex>& srcVertices,
                                 const std::vector<uint32_t>& srcIndices,
                                 const std::vector<uint32_t>& selectedFaces);

// --- edge extrude (#64) ---------------------------------------------------
// Pull selected edges to grow new quad faces — the edge sibling of the face
// extrude. The original edge stays on the surface; a fresh edge is duplicated
// from it and the two are bridged by a quad.

// New geometry for an edge-extrude, built at zero offset (the new edge coincides
// with the source edge). The editor slides `movingVerts` along `normal`.
struct EdgeExtrusion {
    std::vector<Vertex> vertices;   // source verts + duplicated bottom/top edge verts
    std::vector<uint32_t> indices;  // source tris + one bridging quad (2 tris) per selected edge
    std::vector<uint32_t> movingVerts; // indices into `vertices` that slide: the new (top) edge
    vec3 normal{0.0f, 1.0f, 0.0f};  // object-space slide direction (normalized)
    // New top-edge endpoint pairs (indices into `vertices`). The editor maps these
    // to EditEdge ids after the snapshot rebuild to keep the new edges selected.
    std::vector<std::pair<uint32_t, uint32_t>> newEdges;
};

// Build pull geometry for an edge selection. Each selected edge spawns a fresh
// edge (duplicated at zero offset) bridged to the original by a quad; edges that
// share an endpoint share the duplicated vertex, so a connected selection pulls
// as one strip. The slide direction is per-edge: a boundary edge (one incident
// face) extends in that face's plane, away from the face; a manifold edge (two
// faces) lifts along their averaged normal (a ridge); contributions are summed
// and normalized. selectedEdges are EditEdge ids. Returns an empty result if the
// selection is empty, refers to a stale id, or the summed direction is degenerate.
EdgeExtrusion BuildEdgeExtrusion(const EditMesh& mesh, const std::vector<Vertex>& srcVertices,
                                 const std::vector<uint32_t>& srcIndices,
                                 const std::vector<uint32_t>& selectedEdges);

// --- subdivide (#62) ------------------------------------------------------
// Add geometry locally: insert edge midpoints and re-triangulate. Selected
// triangles split 1->4 (red); a neighbour that shares a split edge but isn't
// itself selected splits 1->2 or 1->3 (green) so no T-junction / crack is left,
// keeping the surface watertight. Plain midpoints (no Loop smoothing) so adding
// detail doesn't move the surface. GL-free; the editor swaps the result in.

// New geometry for a subdivision. The editor reselects `newVerts` (the inserted
// midpoints) in Vertex mode so they're immediately transformable.
struct MeshSubdivision {
    std::vector<Vertex> vertices;  // source verts + one midpoint per split group-edge
    std::vector<uint32_t> indices; // re-triangulated soup (selected + affected neighbours)
    std::vector<uint32_t> newVerts; // indices into `vertices` of the inserted midpoints
};

// Subdivide the selected faces: every edge of each selected triangle is split at
// its midpoint, and faces around the selection re-triangulate to stay watertight.
// selectedFaces are EditFace ids (== triangle ids). Empty/stale -> empty result.
MeshSubdivision BuildFaceSubdivision(const EditMesh& mesh, const std::vector<Vertex>& srcVertices,
                                     const std::vector<uint32_t>& srcIndices,
                                     const std::vector<uint32_t>& selectedFaces);

// Subdivide the selected edges: each is split at its midpoint and its incident
// faces re-triangulate. selectedEdges are EditEdge ids. Empty/stale -> empty.
MeshSubdivision BuildEdgeSubdivision(const EditMesh& mesh, const std::vector<Vertex>& srcVertices,
                                     const std::vector<uint32_t>& srcIndices,
                                     const std::vector<uint32_t>& selectedEdges);

// --- smooth (#62 T6) ------------------------------------------------------
// Per-vertex neighbour list (group-space) derived from the EditMesh edges, for
// Laplacian smoothing. adjacency[v] = the other endpoint of each edge on v.
std::vector<std::vector<uint32_t>> BuildVertexAdjacency(const EditMesh& mesh);

// Laplacian smooth: for `iterations`, move each selected vertex a fraction
// `strength` toward the average of its neighbours (Jacobi — each pass reads the
// previous pass's positions). `positions` and `adjacency` are parallel by vertex
// id; unselected verts are anchors (kept, but read as neighbours). A vertex with
// no neighbours is left unchanged. strength is clamped to [0,1]. GL-free.
std::vector<vec3> LaplacianSmooth(const std::vector<vec3>& positions,
                                  const std::vector<std::vector<uint32_t>>& adjacency,
                                  const std::vector<uint32_t>& selected, float strength, int iterations);

} // namespace forge
