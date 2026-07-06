#pragma once

#include "forge/core/Geometry.h"
#include "forge/core/Math.h"

#include <cstdint>
#include <vector>

namespace forge {

struct Vertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
};

// Index range drawn with one material slot (#80). Slots index the owning
// entity's materials: 0 = Entity::material, 1+ = Entity::extraMaterials.
// A mesh with no submeshes is single-material (the whole index buffer, slot 0).
struct Submesh {
    uint32_t firstIndex = 0; // offset into the index buffer
    uint32_t indexCount = 0;
    uint32_t materialSlot = 0;
};

// Drops ranges that don't fit the index buffer (or a hostile count of them) so
// a bad file can't drive out-of-bounds draws; an empty result means "treat as
// single-material". Free function so the unit tests cover it without GL.
inline std::vector<Submesh> SanitizeSubmeshes(std::vector<Submesh> submeshes, size_t indexCount)
{
    std::erase_if(submeshes, [indexCount](const Submesh& s) {
        return s.indexCount == 0 || s.firstIndex > indexCount ||
               s.indexCount > indexCount - s.firstIndex; // overflow-safe range check
    });
    // Aggregate bounds, checked after the erase so garbage entries can't veto
    // good ones. More (valid) submeshes than triangles, or ranges covering more
    // indices than the buffer holds (overlaps — every legitimate producer emits
    // a partition), is a hostile or corrupt file: fall back to single-material
    // rather than multiplying draw calls and path-tracer triangle uploads.
    uint64_t covered = 0;
    for (const Submesh& s : submeshes)
        covered += s.indexCount;
    if (submeshes.size() > indexCount / 3 || covered > indexCount)
        submeshes.clear();
    return submeshes;
}

// GPU mesh with retained CPU data — the picker (M2) and the path tracer (M5)
// both need the triangles on the CPU side.
class Mesh {
public:
    Mesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices,
         std::vector<Submesh> submeshes = {});
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    void Draw() const;
    // Draw a sub-range of the index buffer (submesh material slots, #80).
    // indexCount 0 = everything from firstIndex; out-of-range clamps.
    void DrawRange(uint32_t firstIndex, uint32_t indexCount) const;

    const std::vector<Vertex>& Vertices() const { return m_Vertices; }
    const std::vector<uint32_t>& Indices() const { return m_Indices; }
    // Empty = single-material mesh (whole buffer, slot 0). Never mutated after
    // construction, so the scene hash can rely on the mesh pointer alone.
    const std::vector<Submesh>& Submeshes() const { return m_Submeshes; }
    const AABB& Bounds() const { return m_Bounds; }

    // --- sculpting support ---------------------------------------------------
    std::vector<Vertex>& MutableVertices() { return m_Vertices; }
    void UploadVertices(); // push CPU vertices to the GPU, bumps Version
    void RecomputeBounds();
    uint64_t Version() const { return m_Version; } // mixed into the scene hash so the path tracer sees edits

private:
    std::vector<Vertex> m_Vertices;
    std::vector<uint32_t> m_Indices;
    std::vector<Submesh> m_Submeshes; // sanitized at construction, immutable after
    AABB m_Bounds;
    uint64_t m_Version = 0;
    uint32_t m_VAO = 0, m_VBO = 0, m_IBO = 0;
};

} // namespace forge
