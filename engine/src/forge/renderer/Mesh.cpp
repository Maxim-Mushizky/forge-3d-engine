#include "Mesh.h"

#include "forge/core/Log.h"

#include <GL/glew.h>

#include <algorithm>

namespace forge {

Mesh::Mesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices, std::vector<Submesh> submeshes)
    : m_Vertices(std::move(vertices)), m_Indices(std::move(indices)),
      m_Submeshes(SanitizeSubmeshes(std::move(submeshes), m_Indices.size()))
{
    for (const Vertex& v : m_Vertices)
        m_Bounds.Expand(v.position);

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glGenBuffers(1, &m_IBO);

    glBindVertexArray(m_VAO);

    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    // Dynamic: sculpting re-uploads vertices; cost on static meshes is negligible.
    glBufferData(GL_ARRAY_BUFFER, m_Vertices.size() * sizeof(Vertex), m_Vertices.data(), GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_IBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_Indices.size() * sizeof(uint32_t), m_Indices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));

    glBindVertexArray(0);
}

Mesh::~Mesh()
{
    glDeleteBuffers(1, &m_IBO);
    glDeleteBuffers(1, &m_VBO);
    glDeleteVertexArrays(1, &m_VAO);
}

void Mesh::UploadVertices()
{
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(m_Vertices.size() * sizeof(Vertex)), m_Vertices.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    ++m_Version;
}

void Mesh::SetSkin(std::vector<VertexSkin> skin)
{
    if (skin.size() != m_Vertices.size()) {
        // External asset data can be malformed — degrade to unskinned, never assert.
        FORGE_WARN("Mesh::SetSkin: %zu skin entries for %zu vertices — mesh stays unskinned",
                   skin.size(), m_Vertices.size());
        return;
    }
    m_Skin = std::move(skin);
    m_BindVertices = m_Vertices; // vertices must still be the undeformed bind pose here
}

void Mesh::SetMorphTargets(std::vector<MorphTarget> targets)
{
    for (const MorphTarget& t : targets) {
        // External asset data can be malformed — degrade to morphless, never assert.
        // All-or-nothing: a partially-valid target set would desync name->index
        // resolution against the file's targetNames.
        if (t.positionDeltas.size() != m_Vertices.size() ||
            (!t.normalDeltas.empty() && t.normalDeltas.size() != m_Vertices.size())) {
            FORGE_WARN("Mesh::SetMorphTargets: target \"%s\" has %zu/%zu deltas for %zu vertices "
                       "— mesh stays morphless",
                       t.name.c_str(), t.positionDeltas.size(), t.normalDeltas.size(),
                       m_Vertices.size());
            return;
        }
    }
    m_MorphTargets = std::move(targets);
    // Morph-only meshes never pass through SetSkin, so snapshot the bind here
    // too — deformation always re-derives from bind, never compounds. Don't
    // clobber a snapshot the skin already took (vertices may be deformed by now).
    if (m_BindVertices.empty())
        m_BindVertices = m_Vertices; // vertices must still be the undeformed bind pose here
}

void Mesh::RecomputeBounds()
{
    m_Bounds = AABB{};
    for (const Vertex& v : m_Vertices)
        m_Bounds.Expand(v.position);
}

void Mesh::Draw() const
{
    glBindVertexArray(m_VAO);
    glDrawElements(GL_TRIANGLES, (GLsizei)m_Indices.size(), GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

void Mesh::DrawRange(uint32_t firstIndex, uint32_t indexCount) const
{
    if (firstIndex >= m_Indices.size())
        return;
    uint32_t available = (uint32_t)m_Indices.size() - firstIndex;
    uint32_t count = indexCount == 0 ? available : std::min(indexCount, available);
    glBindVertexArray(m_VAO);
    glDrawElements(GL_TRIANGLES, (GLsizei)count, GL_UNSIGNED_INT,
                   (const void*)(uintptr_t)(firstIndex * sizeof(uint32_t)));
    glBindVertexArray(0);
}

} // namespace forge
