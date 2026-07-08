#pragma once

#include "forge/core/Math.h"
#include "forge/renderer/Mesh.h" // Vertex + VertexSkin PODs only — Mesh itself is never touched

#include <vector>

namespace forge {

// Linear-blend skinning over plain vectors, GL-free so it unit-tests headless
// (same shape as ComputeMeshStats: the test binary cannot link Mesh.cpp).
// Positions blend Σ w·palette[j]·p. Normals blend the PER-JOINT inverse
// transposes — inverse-transposing the blended matrix is wrong under
// non-uniform joint scale (unit-tested). UVs copy through. A zero weight sum
// leaves the vertex at bind; out-of-range joint indices are dropped, never
// read. out is resized to bind.size() and may alias bind.
void SkinVertices(const std::vector<Vertex>& bind, const std::vector<VertexSkin>& skin,
                  const std::vector<mat4>& palette, std::vector<Vertex>& out);

} // namespace forge
