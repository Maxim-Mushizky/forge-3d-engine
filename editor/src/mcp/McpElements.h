#pragma once

#include <forge/core/Math.h>
#include <forge/geometry/EditMesh.h>

#include <cstdint>
#include <vector>

namespace forge {

// GL-free element-query kernel for the forge.* script surface (#91). Edit-mode
// ops (extrude/subdivide) take EditMesh element ids; an agent has no cursor to
// pick them with, so these list faces/edges with world-space centroids the
// script can filter on. Same headless-testable pattern as McpViews/McpScript.

struct ElementInfo {
    uint32_t id = 0;
    vec3 center{0.0f};  // world space
    vec3 normal{0.0f};  // world space, faces only (zero for edges)
};

// Faces (id == triangle id == RaycastHit.triIndex / 3), optionally filtered to
// centroids within `radius` of world-space `center` (pass nullptr for all),
// capped at maxCount. `total` receives the pre-cap match count.
std::vector<ElementInfo> ListFaceElements(const EditMesh& mesh, const mat4& world,
                                          const vec3* center, float radius, size_t maxCount,
                                          size_t& total);

// Edges (midpoint as center, normal zero), same filter/cap contract.
std::vector<ElementInfo> ListEdgeElements(const EditMesh& mesh, const mat4& world,
                                          const vec3* center, float radius, size_t maxCount,
                                          size_t& total);

// Deduplicated edge ids bounding the given faces (stale face ids and collapsed
// kNoEdge slots are skipped).
std::vector<uint32_t> EdgesOfFaces(const EditMesh& mesh, const std::vector<uint32_t>& faces);

} // namespace forge
