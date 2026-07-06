#pragma once

#include "forge/renderer/Mesh.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace forge {

// Options for UnwrapUV (#81). Defaults match a 1k texture target; xatlas picks
// texels-per-unit to fit everything on a single page of this resolution.
struct UnwrapOptions {
    uint32_t resolution = 1024; // target atlas size in texels (square)
    uint32_t padding = 2;       // texels between charts, keeps bilinear taps inside
    bool bruteForce = false;    // exhaustive chart packing: better fit, much slower
};

// Result of the pure unwrap kernel. Vertices are the input vertices re-indexed
// by xatlas (seam vertices duplicated with distinct UVs, all other attributes
// copied from the source vertex); triangle order is preserved, so the input
// submesh ranges apply to the output index buffer unchanged.
struct UnwrapResult {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Submesh> submeshes;
    uint32_t chartCount = 0;
    uint32_t atlasWidth = 0;  // texels; UVs are already normalized to [0,1]
    uint32_t atlasHeight = 0;
    float utilization = 0.0f; // packed texel coverage of the atlas page, 0-1
};

// Generate a non-overlapping UV atlas with xatlas. Pure kernel — no GL, no
// Mesh handle — so it unit-tests headless. Returns nullopt when the input is
// empty, structurally invalid, or nothing chartable survives (all-degenerate).
std::optional<UnwrapResult> UnwrapUVData(const std::vector<Vertex>& vertices,
                                         const std::vector<uint32_t>& indices,
                                         const std::vector<Submesh>& submeshes,
                                         const UnwrapOptions& options = {});

// Summed triangle area in UV space. ~atlas utilization for a non-overlapping
// atlas; values well above 1 mean charts overlap (e.g. the primitive cube maps
// every face onto the same unit square and reports ~6).
float UvAreaCoverage(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);

// Mesh-level convenience wrapper for the editor/MCP call sites. Inline so the
// GL-free test binary can compile UvUnwrap.cpp without linking Mesh's GL side.
inline std::shared_ptr<Mesh> UnwrapUV(const Mesh& mesh, const UnwrapOptions& options = {})
{
    auto result = UnwrapUVData(mesh.Vertices(), mesh.Indices(), mesh.Submeshes(), options);
    if (!result)
        return nullptr;
    return std::make_shared<Mesh>(std::move(result->vertices), std::move(result->indices),
                                  std::move(result->submeshes));
}

} // namespace forge
