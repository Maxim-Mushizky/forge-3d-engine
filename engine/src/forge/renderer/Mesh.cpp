#include "Mesh.h"

#include <GL/glew.h>

namespace forge {

Mesh::Mesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices)
    : m_Vertices(std::move(vertices)), m_Indices(std::move(indices))
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

} // namespace forge
