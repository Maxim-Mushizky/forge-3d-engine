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

MeshEdgeMap MeshEdgeMap::Build(const Mesh& mesh, const MeshTopology& topology)
{
    const auto& idx = mesh.Indices();

    MeshEdgeMap map;
    map.lookup.reserve(idx.size());
    auto add = [&](uint32_t a, uint32_t b, uint32_t tri) {
        uint32_t ga = topology.weldGroup[a], gb = topology.weldGroup[b];
        if (ga == gb)
            return; // degenerate edge (both endpoints co-located)
        if (ga > gb)
            std::swap(ga, gb);
        uint64_t key = ((uint64_t)ga << 32) | gb;
        auto [it, inserted] = map.lookup.try_emplace(key, (uint32_t)map.edges.size());
        if (inserted)
            map.edges.push_back({ga, gb, {}});
        map.edges[it->second].tris.push_back(tri);
    };
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        uint32_t tri = (uint32_t)(i / 3);
        add(idx[i], idx[i + 1], tri);
        add(idx[i + 1], idx[i + 2], tri);
        add(idx[i + 2], idx[i], tri);
    }
    return map;
}

const MeshEdgeMap::Edge* MeshEdgeMap::Find(uint32_t groupA, uint32_t groupB) const
{
    if (groupA > groupB)
        std::swap(groupA, groupB);
    auto it = lookup.find(((uint64_t)groupA << 32) | groupB);
    return it == lookup.end() ? nullptr : &edges[it->second];
}

std::shared_ptr<Mesh> MirrorBakeX(const Mesh& mesh)
{
    constexpr float kPlaneEps = 1e-4f;

    std::vector<Vertex> verts = mesh.Vertices();
    const auto& idx = mesh.Indices();

    // mirrorIndex[i] = index of vertex i's mirror image (i itself when on the plane).
    std::vector<uint32_t> mirrorIndex(verts.size());
    size_t originalCount = verts.size();
    for (uint32_t i = 0; i < (uint32_t)originalCount; ++i) {
        if (std::abs(verts[i].position.x) < kPlaneEps) {
            verts[i].position.x = 0.0f; // snap: index-shared seam is watertight by construction
            mirrorIndex[i] = i;
        } else {
            Vertex m = verts[i];
            m.position.x = -m.position.x;
            m.normal.x = -m.normal.x;
            mirrorIndex[i] = (uint32_t)verts.size();
            verts.push_back(m);
        }
    }

    std::vector<uint32_t> indices = mesh.Indices();
    indices.reserve(idx.size() * 2);
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
        // Triangles fully in the plane would mirror onto themselves with flipped
        // winding — a duplicate, internal, z-fighting face. Skip.
        if (mirrorIndex[a] == a && mirrorIndex[b] == b && mirrorIndex[c] == c)
            continue;
        // Reversed winding keeps the mirrored surface outward-facing.
        indices.push_back(mirrorIndex[a]);
        indices.push_back(mirrorIndex[c]);
        indices.push_back(mirrorIndex[b]);
    }

    auto result = std::make_shared<Mesh>(std::move(verts), std::move(indices));
    MeshTopology topo = MeshTopology::Build(*result);
    RecomputeNormalsWelded(*result, topo);
    result->UploadVertices();
    return result;
}

std::shared_ptr<Mesh> LaplacianSmoothMesh(const Mesh& mesh, float strength, int iterations)
{
    std::vector<Vertex> verts = mesh.Vertices();
    std::vector<uint32_t> indices = mesh.Indices();
    MeshTopology topo = MeshTopology::Build(mesh);

    // Jacobi passes over weld groups: snapshot positions first so the result
    // is independent of group iteration order.
    std::vector<vec3> groupPos(topo.groups.size());
    for (int pass = 0; pass < iterations; ++pass) {
        for (size_t g = 0; g < topo.groups.size(); ++g)
            groupPos[g] = verts[topo.groups[g][0]].position;
        for (size_t g = 0; g < topo.groups.size(); ++g) {
            const auto& nbrs = topo.groupNeighbors[g];
            if (nbrs.empty())
                continue;
            vec3 avg{0.0f};
            for (uint32_t n : nbrs)
                avg += groupPos[n];
            avg /= (float)nbrs.size();
            const vec3 target = glm::mix(groupPos[g], avg, strength);
            for (uint32_t vi : topo.groups[g])
                verts[vi].position = target;
        }
    }

    auto result = std::make_shared<Mesh>(std::move(verts), std::move(indices));
    MeshTopology rtopo = MeshTopology::Build(*result);
    RecomputeNormalsWelded(*result, rtopo);
    result->UploadVertices();
    return result;
}

std::shared_ptr<Mesh> LoopSubdivide(const Mesh& mesh, bool keepShape)
{
    MeshTopology topo = MeshTopology::Build(mesh);
    MeshEdgeMap edgeMap = MeshEdgeMap::Build(mesh, topo);

    const auto& verts = mesh.Vertices();
    const auto& idx = mesh.Indices();
    const size_t numGroups = topo.groups.size();

    std::vector<vec3> groupPos(numGroups);
    for (size_t g = 0; g < numGroups; ++g)
        groupPos[g] = verts[topo.groups[g][0]].position;

    // --- odd (edge) vertex positions, in group space -------------------------
    auto oppositeGroup = [&](uint32_t tri, uint32_t g0, uint32_t g1) -> int {
        for (int c = 0; c < 3; ++c) {
            uint32_t g = topo.weldGroup[idx[tri * 3 + c]];
            if (g != g0 && g != g1)
                return (int)g;
        }
        return -1;
    };
    std::vector<vec3> edgePos(edgeMap.edges.size());
    for (size_t ei = 0; ei < edgeMap.edges.size(); ++ei) {
        const auto& e = edgeMap.edges[ei];
        vec3 mid = (groupPos[e.g0] + groupPos[e.g1]) * 0.5f;
        // Boundary (1 tri) and non-manifold (>2 tris) edges fall back to midpoint.
        if (keepShape || e.tris.size() != 2) {
            edgePos[ei] = mid;
            continue;
        }
        int l = oppositeGroup(e.tris[0], e.g0, e.g1);
        int r = oppositeGroup(e.tris[1], e.g0, e.g1);
        edgePos[ei] = (l < 0 || r < 0) ? mid
                                       : (groupPos[e.g0] + groupPos[e.g1]) * (3.0f / 8.0f) +
                                             (groupPos[l] + groupPos[r]) * (1.0f / 8.0f);
    }

    // --- even (original) vertex positions ------------------------------------
    std::vector<vec3> evenPos = groupPos;
    if (!keepShape) {
        std::vector<std::vector<uint32_t>> boundaryNbr(numGroups);
        for (const auto& e : edgeMap.edges) {
            if (e.tris.size() == 1) {
                boundaryNbr[e.g0].push_back(e.g1);
                boundaryNbr[e.g1].push_back(e.g0);
            }
        }
        for (size_t g = 0; g < numGroups; ++g) {
            if (!boundaryNbr[g].empty()) {
                // Boundary rule; corners/non-manifold boundary verts stay pinned.
                if (boundaryNbr[g].size() == 2)
                    evenPos[g] = groupPos[g] * 0.75f +
                                 (groupPos[boundaryNbr[g][0]] + groupPos[boundaryNbr[g][1]]) * 0.125f;
                continue;
            }
            const auto& nbrs = topo.groupNeighbors[g];
            size_t k = nbrs.size();
            if (k < 3)
                continue;
            // Warren's beta: cheaper than the original Loop weights, same limit smoothness class.
            float beta = k == 3 ? 3.0f / 16.0f : 3.0f / (8.0f * (float)k);
            vec3 sum(0.0f);
            for (uint32_t n : nbrs)
                sum += groupPos[n];
            evenPos[g] = groupPos[g] * (1.0f - (float)k * beta) + sum * beta;
        }
    }

    // --- output: connectivity & UVs on RAW indices ----------------------------
    // Seam copies of an edge have different raw indices -> two output verts with
    // their own UVs, but both read the same group-space position: no cracks.
    std::vector<Vertex> outVerts(verts.size());
    for (size_t i = 0; i < verts.size(); ++i) {
        outVerts[i] = verts[i];
        outVerts[i].position = evenPos[topo.weldGroup[i]];
    }

    std::unordered_map<uint64_t, uint32_t> rawEdgeVert; // (rawMin<<32 | rawMax) -> out index
    rawEdgeVert.reserve(idx.size());
    auto edgeVert = [&](uint32_t a, uint32_t b) -> uint32_t {
        uint64_t key = a < b ? ((uint64_t)a << 32 | b) : ((uint64_t)b << 32 | a);
        auto [it, inserted] = rawEdgeVert.try_emplace(key, (uint32_t)outVerts.size());
        if (inserted) {
            const MeshEdgeMap::Edge* e = edgeMap.Find(topo.weldGroup[a], topo.weldGroup[b]);
            Vertex v{};
            v.position = e ? edgePos[(size_t)(e - edgeMap.edges.data())]
                           : (verts[a].position + verts[b].position) * 0.5f;
            v.normal = vec3(0.0f, 1.0f, 0.0f); // recomputed below
            v.uv = (verts[a].uv + verts[b].uv) * 0.5f;
            outVerts.push_back(v);
        }
        return it->second;
    };

    std::vector<uint32_t> outIdx;
    outIdx.reserve(idx.size() * 4);
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
        uint32_t ga = topo.weldGroup[a], gb = topo.weldGroup[b], gc = topo.weldGroup[c];
        if (ga == gb || gb == gc || gc == ga)
            continue; // degenerate
        uint32_t eab = edgeVert(a, b), ebc = edgeVert(b, c), eca = edgeVert(c, a);
        const uint32_t quads[4][3] = {{a, eab, eca}, {eab, b, ebc}, {eca, ebc, c}, {eab, ebc, eca}};
        for (const auto& t : quads) {
            outIdx.push_back(t[0]);
            outIdx.push_back(t[1]);
            outIdx.push_back(t[2]);
        }
    }

    auto result = std::make_shared<Mesh>(std::move(outVerts), std::move(outIdx));
    MeshTopology newTopo = MeshTopology::Build(*result);
    RecomputeNormalsWelded(*result, newTopo);
    result->UploadVertices();
    return result;
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

void RecomputeNormalsFlat(Mesh& mesh)
{
    auto& verts = mesh.MutableVertices();
    const auto& idx = mesh.Indices();
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        const vec3& p0 = verts[idx[i]].position;
        vec3 n = glm::cross(verts[idx[i + 1]].position - p0, verts[idx[i + 2]].position - p0);
        float len = glm::length(n);
        n = len > 1e-8f ? n / len : vec3(0.0f, 1.0f, 0.0f);
        verts[idx[i]].normal = n;
        verts[idx[i + 1]].normal = n;
        verts[idx[i + 2]].normal = n;
    }
}

} // namespace forge
