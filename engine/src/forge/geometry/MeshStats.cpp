#include "MeshStats.h"

#include <cstring>
#include <unordered_map>

namespace forge {

namespace {

// Exact-bit position key: welding is only meant to undo UV-seam duplication,
// where the duplicated vertices are byte-identical copies.
struct PosKey {
    uint32_t x, y, z;
    bool operator==(const PosKey&) const = default;
};

struct PosKeyHash {
    size_t operator()(const PosKey& k) const
    {
        uint64_t h = 1469598103934665603ull; // FNV-1a
        for (uint32_t v : {k.x, k.y, k.z}) {
            h ^= v;
            h *= 1099511628211ull;
        }
        return (size_t)h;
    }
};

PosKey KeyOf(const vec3& p)
{
    PosKey k;
    std::memcpy(&k, &p, sizeof(k));
    return k;
}

} // namespace

MeshStats ComputeMeshStats(const std::vector<Vertex>& vertices,
                           const std::vector<uint32_t>& indices)
{
    MeshStats s;
    s.vertexCount = (uint32_t)vertices.size();
    s.triangleCount = (uint32_t)(indices.size() / 3);

    // Weld coincident positions so seam-duplicated meshes read as closed.
    std::unordered_map<PosKey, uint32_t, PosKeyHash> weld;
    std::vector<uint32_t> welded(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        auto [it, inserted] = weld.try_emplace(KeyOf(vertices[i].position), (uint32_t)i);
        welded[i] = it->second;

        s.bounds.Expand(vertices[i].position);
        if (!s.hasUVs && (vertices[i].uv.x != 0.0f || vertices[i].uv.y != 0.0f))
            s.hasUVs = true;
    }

    // Undirected welded edge -> triangle use count.
    std::unordered_map<uint64_t, uint32_t> edgeUse;
    edgeUse.reserve(indices.size());
    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
        const uint32_t a = welded[indices[t]], b = welded[indices[t + 1]],
                       c = welded[indices[t + 2]];

        const vec3& pa = vertices[a].position;
        const vec3 cross = glm::cross(vertices[b].position - pa, vertices[c].position - pa);
        if (a == b || b == c || a == c || glm::dot(cross, cross) < 1e-20f) {
            // Degenerates contribute no surface; counting their edges would
            // misreport the surrounding topology as non-manifold.
            ++s.degenerateTriangles;
            continue;
        }

        for (auto [lo, hi] : {std::pair{a, b}, {b, c}, {a, c}}) {
            if (lo > hi)
                std::swap(lo, hi);
            ++edgeUse[((uint64_t)lo << 32) | hi];
        }
    }

    for (const auto& [edge, uses] : edgeUse) {
        if (uses == 1)
            ++s.boundaryEdges;
        else if (uses > 2)
            ++s.nonManifoldEdges;
    }
    s.watertight = s.triangleCount > s.degenerateTriangles && s.boundaryEdges == 0 &&
                   s.nonManifoldEdges == 0;
    return s;
}

} // namespace forge
