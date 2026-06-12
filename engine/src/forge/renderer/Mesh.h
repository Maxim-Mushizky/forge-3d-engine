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

// GPU mesh with retained CPU data — the picker (M2) and the path tracer (M5)
// both need the triangles on the CPU side.
class Mesh {
public:
    Mesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;

    void Draw() const;

    const std::vector<Vertex>& Vertices() const { return m_Vertices; }
    const std::vector<uint32_t>& Indices() const { return m_Indices; }
    const AABB& Bounds() const { return m_Bounds; }

    // --- sculpting support ---------------------------------------------------
    std::vector<Vertex>& MutableVertices() { return m_Vertices; }
    void UploadVertices(); // push CPU vertices to the GPU, bumps Version
    void RecomputeBounds();
    uint64_t Version() const { return m_Version; } // mixed into the scene hash so the path tracer sees edits

private:
    std::vector<Vertex> m_Vertices;
    std::vector<uint32_t> m_Indices;
    AABB m_Bounds;
    uint64_t m_Version = 0;
    uint32_t m_VAO = 0, m_VBO = 0, m_IBO = 0;
};

} // namespace forge
