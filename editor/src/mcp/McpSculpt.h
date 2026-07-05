#pragma once

#include <forge/core/Math.h>
#include <forge/geometry/MeshEdit.h>
#include <forge/renderer/Mesh.h>

#include <cstddef>
#include <vector>

namespace forge {

// GL-free sculpt kernel for the forge.* script surface (#105): the brush math
// behind forge.sculpt / forge.move_verts. Everything operates in OBJECT space
// on welded vertex groups — every raw copy of a seam vertex moves together, so
// UV/normal seams (sphere longitude, cube corners) never tear. The caller owns
// world->object conversion and the normals/bounds/VBO recompute afterwards.
// Same headless-testable pattern as McpElements/McpViews.

enum class SculptFalloff { Smooth, Linear, Constant };

// Brush weight for normalized distance t = dist/radius: 1 at the center, 0 at
// and beyond the radius. Smooth is the interactive SculptTool curve
// (1 - smoothstep); Constant is a hard-edged cylinder of weight 1.
float FalloffWeight(SculptFalloff falloff, float t);

// Each op returns the number of weld groups it moved (0 = the brush missed
// the mesh entirely; the caller can skip normals/upload/undo). Targets are
// computed from pre-move positions, so results never depend on group order.
// Groups referencing out-of-range vertices (stale topology) are skipped.

// Translate everything within `radius` of `center` by offset * weight.
// forge.move_verts and the grab brush are both this.
size_t SculptMove(std::vector<Vertex>& verts, const MeshTopology& topo, const vec3& center,
                  float radius, const vec3& offset, SculptFalloff falloff);

// Puff the surface along per-vertex normals by amount * weight (negative
// amount dents inward). Assumes normals are current — the caller refreshes
// them between repeated strokes.
size_t SculptInflate(std::vector<Vertex>& verts, const MeshTopology& topo, const vec3& center,
                     float radius, float amount);

// Laplacian relax toward the neighbor-group average, mix factor
// clamp(weight * strength, 0, 1). Same shape as the interactive Smooth brush.
size_t SculptSmooth(std::vector<Vertex>& verts, const MeshTopology& topo, const vec3& center,
                    float radius, float strength);

// Moves `point` to the nearest weld-group representative position (#110
// snap): brush centers guessed off the live surface land ON it instead of
// hovering uselessly beside it. False when the topology has no valid groups.
bool SnapToNearestVertex(const std::vector<Vertex>& verts, const MeshTopology& topo, vec3& point);

// Fraction [0,1] of brush-affected groups whose normal points TOWARD
// `interior` (folded/inside-out surface detector, #110): stacked dents can
// invert a region, after which normal-following brushes push the wrong way.
// `interior` is any point inside the mesh (its AABB center works). Returns 0
// for an empty region.
float InvertedNormalFraction(const std::vector<Vertex>& verts, const MeshTopology& topo,
                             const vec3& center, float radius, const vec3& interior);

} // namespace forge
