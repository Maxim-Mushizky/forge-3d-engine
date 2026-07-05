#include "McpElements.h"

#include <algorithm>
#include <unordered_set>

namespace forge {

// Shared filter/cap walk: `worldPoint` maps an element to its world center.
template <typename PointFn, typename EmitFn>
static void FilterElements(size_t count, const vec3* center, float radius, size_t maxCount,
                           size_t& total, PointFn worldPoint, EmitFn emit)
{
    total = 0;
    for (size_t i = 0; i < count; ++i) {
        const vec3 p = worldPoint(i);
        if (center && glm::length(p - *center) > radius)
            continue;
        ++total;
        if (total <= maxCount)
            emit((uint32_t)i, p);
    }
}

std::vector<ElementInfo> ListFaceElements(const EditMesh& mesh, const mat4& world,
                                          const vec3* center, float radius, size_t maxCount,
                                          size_t& total)
{
    std::vector<ElementInfo> out;
    // Normals transform by the inverse-transpose (plain mat3 skews them under
    // non-uniform scale — scripts filter on normal.y to find "top" faces, so
    // this must be exact, not uniform-scale-approximate).
    const mat3 normalMat = glm::transpose(glm::inverse(mat3(world)));
    FilterElements(
        mesh.faces.size(), center, radius, maxCount, total,
        [&](size_t i) { return vec3(world * vec4(mesh.faces[i].centroid, 1.0f)); },
        [&](uint32_t id, const vec3& p) {
            const vec3 n = normalMat * mesh.faces[id].normal;
            const float len = glm::length(n);
            out.push_back({id, p, len > 1e-8f ? n / len : vec3(0.0f)});
        });
    return out;
}

std::vector<ElementInfo> ListEdgeElements(const EditMesh& mesh, const mat4& world,
                                          const vec3* center, float radius, size_t maxCount,
                                          size_t& total)
{
    std::vector<ElementInfo> out;
    FilterElements(
        mesh.edges.size(), center, radius, maxCount, total,
        [&](size_t i) {
            const EditEdge& e = mesh.edges[i];
            const vec3 mid =
                (mesh.vertices[e.v0].position + mesh.vertices[e.v1].position) * 0.5f;
            return vec3(world * vec4(mid, 1.0f));
        },
        [&](uint32_t id, const vec3& p) { out.push_back({id, p, vec3(0.0f)}); });
    return out;
}

std::vector<uint32_t> EdgesOfFaces(const EditMesh& mesh, const std::vector<uint32_t>& faces)
{
    std::vector<uint32_t> out;
    std::unordered_set<uint32_t> seen;
    for (uint32_t f : faces) {
        if (f >= mesh.faces.size())
            continue; // stale id: skip rather than fail the whole query
        for (uint32_t e : mesh.faces[f].edges)
            if (e != kNoEdge && seen.insert(e).second)
                out.push_back(e);
    }
    return out;
}

} // namespace forge
