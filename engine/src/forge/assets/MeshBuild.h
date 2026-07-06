#pragma once

#include "forge/renderer/Mesh.h"

#include <cstdint>
#include <vector>

namespace forge {

// GL-free mesh-generation kernels for the lathe & sweep primitives (#111).
// They fill raw vertex/index buffers so unit tests can link them without a GL
// context; MeshFactory wraps the result into a Mesh. Same headless pattern as
// the sculpt kernel (McpSculpt) and MeshEdit's pure helpers.
//
// Watertightness contract: seam vertices (the duplicated UV row at angle 0 /
// 2pi) reuse the exact float positions of their partners, so position-welded
// consumers (RecomputeNormalsWelded, MeshStats) see one vertex, not a phantom
// boundary (#117). Caps rebuild their ring from the same expressions as the
// wall for the same reason.

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// Revolve a 2D profile of (radius, height) points, ordered bottom to top,
// around the local +Y axis. Radii must be >= 0; a point at r = 0 is a pole
// and collapses its ring (sphere-style: only the non-degenerate half of each
// adjacent quad is emitted). `closed` adds fan caps over any end whose radius
// is > 0 — ends already at r = 0 are sealed by the pole collapse, and a
// profile that loops back to its first point closes itself (caps are skipped;
// stacking them on the welded seam ring would go non-manifold). Triangles
// wind counter-clockwise seen from outside (#116: winding is what
// RecomputeNormalsWelded rebuilds normals from after any edit).
// Returns false and leaves `out` empty for degenerate or oversized input:
// fewer than two distinct points, every radius zero, a negative radius,
// non-finite values, more than 4096 profile points, or a projected ring grid
// over 2M vertices (builds run serially on the GL main thread).
bool BuildLathe(const std::vector<vec2>& profile, // (r, y) pairs, bottom -> top
                uint32_t sectors,                 // clamped to [3, 1024]
                bool closed, MeshData& out);

// Extrude a closed 2D cross-section along a 3D polyline using
// parallel-transport frames (the frame rotates only as much as the tangent
// does, so the section never twists around the path). The section winding is
// normalized internally, so callers may pass it either way around. Both ends
// are capped with a fan around the section centroid — sections must be
// star-shaped about their centroid (circles, rectangles, rounded profiles),
// which covers handles, aprons and rails.
// Returns false for degenerate or oversized input: fewer than three distinct
// section points, zero section area, fewer than two distinct path points,
// non-finite values, more than 4096 section / 16384 path points, or a
// projected ring grid over 2M vertices.
bool BuildSweep(const std::vector<vec2>& profile, // closed cross-section
                const std::vector<vec3>& path,    // polyline, >= 2 points
                MeshData& out);

} // namespace forge
