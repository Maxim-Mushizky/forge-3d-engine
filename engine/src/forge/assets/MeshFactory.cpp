#include "MeshFactory.h"

#include <glm/gtc/constants.hpp>

namespace forge {

std::shared_ptr<Mesh> MeshFactory::Cube()
{
    // 24 vertices: each face has its own normal.
    static const vec3 faceNormals[6] = {
        {0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0},
    };

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(24);
    indices.reserve(36);

    for (uint32_t f = 0; f < 6; ++f) {
        vec3 n = faceNormals[f];
        // Build a tangent basis for the face.
        vec3 u = (std::abs(n.y) > 0.9f) ? vec3(1, 0, 0) : glm::normalize(glm::cross(vec3(0, 1, 0), n));
        vec3 v = glm::cross(n, u);

        uint32_t base = (uint32_t)vertices.size();
        const vec2 corners[4] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
        for (int c = 0; c < 4; ++c) {
            vec3 pos = n * 0.5f + u * corners[c].x + v * corners[c].y;
            vertices.push_back({pos, n, corners[c] + vec2(0.5f)});
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

std::shared_ptr<Mesh> MeshFactory::Sphere(uint32_t rings, uint32_t sectors)
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve((rings + 1) * (sectors + 1));

    const float pi = glm::pi<float>();
    for (uint32_t r = 0; r <= rings; ++r) {
        float phi = pi * (float)r / (float)rings; // 0..pi from north pole
        for (uint32_t s = 0; s <= sectors; ++s) {
            float theta = 2.0f * pi * (float)s / (float)sectors;
            vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            vertices.push_back({n * 0.5f, n, {(float)s / sectors, 1.0f - (float)r / rings}});
        }
    }

    for (uint32_t r = 0; r < rings; ++r) {
        for (uint32_t s = 0; s < sectors; ++s) {
            uint32_t i0 = r * (sectors + 1) + s;
            uint32_t i1 = i0 + sectors + 1;
            indices.insert(indices.end(), {i0, i1, i0 + 1, i0 + 1, i1, i1 + 1});
        }
    }

    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

std::shared_ptr<Mesh> MeshFactory::Cylinder(uint32_t sectors)
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    const float pi = glm::pi<float>();
    const float r = 0.5f, h = 0.5f;

    // Side wall.
    for (uint32_t s = 0; s <= sectors; ++s) {
        float theta = 2.0f * pi * (float)s / (float)sectors;
        vec3 n{std::cos(theta), 0.0f, std::sin(theta)};
        float u = (float)s / sectors;
        vertices.push_back({{n.x * r, h, n.z * r}, n, {u, 1}});
        vertices.push_back({{n.x * r, -h, n.z * r}, n, {u, 0}});
    }
    for (uint32_t s = 0; s < sectors; ++s) {
        uint32_t i = s * 2;
        indices.insert(indices.end(), {i, i + 2, i + 1, i + 1, i + 2, i + 3});
    }

    // Caps (fan around a center vertex).
    for (int cap = 0; cap < 2; ++cap) {
        float y = cap == 0 ? h : -h;
        vec3 n{0.0f, cap == 0 ? 1.0f : -1.0f, 0.0f};
        uint32_t center = (uint32_t)vertices.size();
        vertices.push_back({{0, y, 0}, n, {0.5f, 0.5f}});
        for (uint32_t s = 0; s <= sectors; ++s) {
            float theta = 2.0f * pi * (float)s / (float)sectors;
            vertices.push_back({{std::cos(theta) * r, y, std::sin(theta) * r}, n,
                                {0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta)}});
        }
        for (uint32_t s = 0; s < sectors; ++s) {
            if (cap == 0)
                indices.insert(indices.end(), {center, center + 2 + s, center + 1 + s});
            else
                indices.insert(indices.end(), {center, center + 1 + s, center + 2 + s});
        }
    }

    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

std::shared_ptr<Mesh> MeshFactory::Cone(uint32_t sectors)
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    const float pi = glm::pi<float>();
    const float r = 0.5f, h = 0.5f;
    // Slope normal: for apex at +h and rim radius r, normal tilts up by atan(r / 2h).
    const float ny = r / (2.0f * h);

    // Side: one apex vertex per sector for correct normals.
    for (uint32_t s = 0; s <= sectors; ++s) {
        float theta = 2.0f * pi * ((float)s + 0.5f) / (float)sectors;
        vec3 n = glm::normalize(vec3(std::cos(theta), ny, std::sin(theta)));
        vertices.push_back({{0, h, 0}, n, {(float)s / sectors, 1}});
    }
    uint32_t rimStart = (uint32_t)vertices.size();
    for (uint32_t s = 0; s <= sectors; ++s) {
        float theta = 2.0f * pi * (float)s / (float)sectors;
        vec3 n = glm::normalize(vec3(std::cos(theta), ny, std::sin(theta)));
        vertices.push_back({{std::cos(theta) * r, -h, std::sin(theta) * r}, n, {(float)s / sectors, 0}});
    }
    for (uint32_t s = 0; s < sectors; ++s)
        indices.insert(indices.end(), {s, rimStart + s + 1, rimStart + s});

    // Base cap.
    uint32_t center = (uint32_t)vertices.size();
    vertices.push_back({{0, -h, 0}, {0, -1, 0}, {0.5f, 0.5f}});
    for (uint32_t s = 0; s <= sectors; ++s) {
        float theta = 2.0f * pi * (float)s / (float)sectors;
        vertices.push_back({{std::cos(theta) * r, -h, std::sin(theta) * r}, {0, -1, 0},
                            {0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta)}});
    }
    for (uint32_t s = 0; s < sectors; ++s)
        indices.insert(indices.end(), {center, center + 1 + s, center + 2 + s});

    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

std::shared_ptr<Mesh> MeshFactory::Torus(float minorRadius, uint32_t majorSectors, uint32_t minorSectors)
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    const float pi = glm::pi<float>();
    const float R = 0.5f - minorRadius; // overall diameter stays ~1

    for (uint32_t i = 0; i <= majorSectors; ++i) {
        float u = 2.0f * pi * (float)i / (float)majorSectors;
        vec3 ringCenter{std::cos(u) * R, 0.0f, std::sin(u) * R};
        for (uint32_t j = 0; j <= minorSectors; ++j) {
            float v = 2.0f * pi * (float)j / (float)minorSectors;
            vec3 n = glm::normalize(vec3(std::cos(u) * std::cos(v), std::sin(v), std::sin(u) * std::cos(v)));
            vertices.push_back({ringCenter + n * minorRadius, n,
                                {(float)i / majorSectors, (float)j / minorSectors}});
        }
    }

    for (uint32_t i = 0; i < majorSectors; ++i) {
        for (uint32_t j = 0; j < minorSectors; ++j) {
            uint32_t i0 = i * (minorSectors + 1) + j;
            uint32_t i1 = i0 + minorSectors + 1;
            indices.insert(indices.end(), {i0, i0 + 1, i1, i1, i0 + 1, i1 + 1});
        }
    }

    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

std::shared_ptr<Mesh> MeshFactory::Plane(float size, uint32_t subdivisions)
{
    uint32_t n = std::max(subdivisions, 1u);
    float h = size * 0.5f;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve((n + 1) * (n + 1));

    for (uint32_t z = 0; z <= n; ++z) {
        for (uint32_t x = 0; x <= n; ++x) {
            float u = (float)x / n, v = (float)z / n;
            vertices.push_back({{-h + u * size, 0.0f, -h + v * size}, {0, 1, 0}, {u, v}});
        }
    }
    for (uint32_t z = 0; z < n; ++z) {
        for (uint32_t x = 0; x < n; ++x) {
            uint32_t i0 = z * (n + 1) + x;
            uint32_t i1 = i0 + n + 1;
            indices.insert(indices.end(), {i0, i1, i0 + 1, i0 + 1, i1, i1 + 1});
        }
    }
    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

} // namespace forge
