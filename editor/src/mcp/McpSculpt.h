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
// hovering uselessly beside it. `sideSignX` restricts candidates to one side
// of the local YZ plane (+1 / -1; on-plane verts always qualify; 0 = any) so
// a mirrored snap cannot wander back across the plane and land a rogue second
// stroke on the primary side. False when no candidate qualifies.
bool SnapToNearestVertex(const std::vector<Vertex>& verts, const MeshTopology& topo, vec3& point,
                         int sideSignX = 0);

// Mirrored move (#110): both applications computed from PRE-move positions
// and each group written once — the mirrored side wins on overlap, matching
// the interactive X-mirror's last-write-wins, instead of summing to a doubled
// stroke along the mirror plane. Returns distinct groups moved.
size_t SculptMoveMirrored(std::vector<Vertex>& verts, const MeshTopology& topo,
                          const vec3& centerA, const vec3& offsetA, const vec3& centerB,
                          const vec3& offsetB, float radius, SculptFalloff falloff);

// Fraction [0,1] of brush-affected groups whose normal points TOWARD
// `interior` (folded/inside-out surface detector, #110): stacked dents can
// invert a region, after which normal-following brushes push the wrong way.
// `interior` is any point inside the mesh (its AABB center works). Returns 0
// for an empty region.
float InvertedNormalFraction(const std::vector<Vertex>& verts, const MeshTopology& topo,
                             const vec3& center, float radius, const vec3& interior);

} // namespace forge
