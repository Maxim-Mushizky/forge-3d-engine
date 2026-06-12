#include "MeshEdit.h"

#include <algorithm>
#include <unordered_map>

namespace forge {

namespace {

struct CellKey {
    int64_t x, y, z;
    bool operator==(const CellKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct CellKeyHash {
    size_t operator()(const CellKey& k) const
    {
        return (size_t)(k.x * 73856093ll ^ k.y * 19349663ll ^ k.z * 83492791ll);
    }
};

CellKey Quantize(const vec3& p)
{
    constexpr float kScale = 1e4f;
    return {(int64_t)std::llround(p.x * kScale), (int64_t)std::llround(p.y * kScale),
            (int64_t)std::llround(p.z * kScale)};
}

} // namespace

MeshTopology MeshTopology::Build(const Mesh& mesh)
{
    const auto& verts = mesh.Vertices();
    const auto& idx = mesh.Indices();

    MeshTopology topo;
    topo.weldGroup.resize(verts.size());

    std::unordered_map<CellKey, uint32_t, CellKeyHash> cellToGroup;
    cellToGroup.reserve(verts.size());
    for (uint32_t i = 0; i < (uint32_t)verts.size(); ++i) {
        CellKey key = Quantize(verts[i].position);
        auto [it, inserted] = cellToGroup.try_emplace(key, (uint32_t)topo.groups.size());
        if (inserted)
            topo.groups.emplace_back();
        topo.weldGroup[i] = it->second;
        topo.groups[it->second].push_back(i);
    }

    // Group adjacency from triangle edges.
    topo.groupNeighbors.resize(topo.groups.size());
    auto link = [&](uint32_t a, uint32_t b) {
        uint32_t ga = topo.weldGroup[a], gb = topo.weldGroup[b];
        if (ga == gb)
            return;
        topo.groupNeighbors[ga].push_back(gb);
        topo.groupNeighbors[gb].push_back(ga);
    };
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        link(idx[i], idx[i + 1]);
        link(idx[i + 1], idx[i + 2]);
        link(idx[i + 2], idx[i]);
    }
    for (auto& n : topo.groupNeighbors) {
        std::sort(n.begin(), n.end());
        n.erase(std::unique(n.begin(), n.end()), n.end());
    }
    return topo;
}

void RecomputeNormalsWelded(Mesh& mesh, const MeshTopology& topology)
{
    auto& verts = mesh.MutableVertices();
    const auto& idx = mesh.Indices();

    std::vector<vec3> groupNormal(topology.groups.size(), vec3(0.0f));
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const vec3& p0 = verts[idx[i]].position;
        vec3 n = glm::cross(verts[idx[i + 1]].position - p0, verts[idx[i + 2]].position - p0);
        groupNormal[topology.weldGroup[idx[i]]] += n;
        groupNormal[topology.weldGroup[idx[i + 1]]] += n;
        groupNormal[topology.weldGroup[idx[i + 2]]] += n;
    }
    for (size_t v = 0; v < verts.size(); ++v) {
        vec3 n = groupNormal[topology.weldGroup[v]];
        float len = glm::length(n);
        verts[v].normal = len > 1e-8f ? n / len : vec3(0, 1, 0);
    }
}

} // namespace forge
