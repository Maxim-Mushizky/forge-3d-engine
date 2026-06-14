#include "EditMesh.h"

#include "forge/core/Log.h"

#include <algorithm>
#include <cmath>

namespace forge {

namespace {

// Weld key: positions snap to a 1e-4 grid, matching MeshTopology so EditMesh
// groups verts identically to the sculpt/edit topology elsewhere in the engine.
struct Cell {
    int64_t x, y, z;
    bool operator==(const Cell& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct CellHash {
    size_t operator()(const Cell& c) const
    {
        uint64_t h = 1469598103934665603ull; // FNV-1a over the three axes
        for (int64_t v : {c.x, c.y, c.z}) {
            h ^= (uint64_t)v;
            h *= 1099511628211ull;
        }
        return (size_t)h;
    }
};

Cell Quantize(const vec3& p)
{
    constexpr float kScale = 1e4f; // 1e-4 weld epsilon
    return {(int64_t)std::llround(p.x * kScale), (int64_t)std::llround(p.y * kScale),
            (int64_t)std::llround(p.z * kScale)};
}

} // namespace

EditMesh BuildEditMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
{
    EditMesh em;

    // Triangle index data is an invariant of every mesh the engine builds; a
    // ragged or out-of-range index buffer is a bug at the call site, not a
    // runtime condition — assert per the no-exceptions house rule.
    FORGE_ASSERT(indices.size() % 3 == 0, "EditMesh needs triangle indices, got %zu", indices.size());

    // 1. Weld co-located raw verts into groups (the EditMesh vertices).
    std::unordered_map<Cell, uint32_t, CellHash> cellToVert;
    std::vector<uint32_t> rawToVert(vertices.size(), 0);
    for (uint32_t i = 0; i < vertices.size(); ++i) {
        Cell key = Quantize(vertices[i].position);
        auto it = cellToVert.find(key);
        uint32_t v;
        if (it == cellToVert.end()) {
            v = (uint32_t)em.vertices.size();
            cellToVert.emplace(key, v);
            em.vertices.push_back(EditVertex{vec3(0.0f), {}, {}, {}});
        } else {
            v = it->second;
        }
        rawToVert[i] = v;
        em.vertices[v].rawVerts.push_back(i);
    }
    // Representative position = centroid of the welded raw verts. They quantize
    // to the same 1e-4 cell so this only averages out sub-epsilon float noise,
    // but it keeps the group's position independent of raw-vertex ordering.
    for (EditVertex& vert : em.vertices) {
        vec3 acc(0.0f);
        for (uint32_t raw : vert.rawVerts)
            acc += vertices[raw].position;
        vert.position = acc / (float)vert.rawVerts.size();
    }

    // 2. Faces (triangles) with object-space normal + centroid.
    uint32_t triCount = (uint32_t)(indices.size() / 3);
    em.faces.reserve(triCount);
    for (uint32_t t = 0; t < triCount; ++t) {
        uint32_t i0 = indices[t * 3], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
        FORGE_ASSERT(i0 < vertices.size() && i1 < vertices.size() && i2 < vertices.size(),
                     "EditMesh index out of range (%u verts)", (uint32_t)vertices.size());
        const vec3& p0 = vertices[i0].position;
        const vec3& p1 = vertices[i1].position;
        const vec3& p2 = vertices[i2].position;
        vec3 n = glm::cross(p1 - p0, p2 - p0); // engine winding convention
        float len = glm::length(n);

        EditFace face;
        face.v[0] = rawToVert[i0];
        face.v[1] = rawToVert[i1];
        face.v[2] = rawToVert[i2];
        face.edges[0] = face.edges[1] = face.edges[2] = kNoEdge;
        face.centroid = (p0 + p1 + p2) / 3.0f;
        face.normal = len > 1e-8f ? n / len : vec3(0.0f, 1.0f, 0.0f);
        uint32_t f = (uint32_t)em.faces.size();
        em.faces.push_back(face);

        for (uint32_t c = 0; c < 3; ++c)
            em.vertices[face.v[c]].faces.push_back(f);
    }

    // 3. Edges in group space. A degenerate corner pair (both verts welded into
    //    one group) contributes no edge — leave the face's slot as kNoEdge.
    auto addEdge = [&](uint32_t a, uint32_t b, uint32_t face) -> uint32_t {
        if (a == b)
            return kNoEdge;
        uint32_t lo = std::min(a, b), hi = std::max(a, b);
        uint64_t k = ((uint64_t)lo << 32) | hi;
        auto it = em.edgeLookup.find(k);
        uint32_t e;
        if (it == em.edgeLookup.end()) {
            e = (uint32_t)em.edges.size();
            em.edgeLookup.emplace(k, e);
            EditEdge edge;
            edge.v0 = lo;
            edge.v1 = hi;
            edge.faces.push_back(face);
            em.edges.push_back(std::move(edge));
        } else {
            e = it->second;
            em.edges[e].faces.push_back(face);
        }
        return e;
    };
    for (uint32_t f = 0; f < em.faces.size(); ++f) {
        EditFace& face = em.faces[f];
        face.edges[0] = addEdge(face.v[0], face.v[1], f);
        face.edges[1] = addEdge(face.v[1], face.v[2], f);
        face.edges[2] = addEdge(face.v[2], face.v[0], f);
    }

    // 4. Per-vertex incident edges.
    for (uint32_t e = 0; e < em.edges.size(); ++e) {
        em.vertices[em.edges[e].v0].edges.push_back(e);
        em.vertices[em.edges[e].v1].edges.push_back(e);
    }

    // 5. Classify edges + dihedral angle between adjacent faces.
    for (EditEdge& edge : em.edges) {
        size_t n = edge.faces.size();
        edge.kind = n == 1 ? EdgeKind::Boundary : n == 2 ? EdgeKind::Manifold : EdgeKind::NonManifold;
        if (edge.kind == EdgeKind::Manifold) {
            const vec3& n0 = em.faces[edge.faces[0]].normal;
            const vec3& n1 = em.faces[edge.faces[1]].normal;
            edge.dihedral = std::acos(glm::clamp(glm::dot(n0, n1), -1.0f, 1.0f));
        }
    }

    return em;
}

EditMesh BuildEditMesh(const Mesh& mesh)
{
    // Accessors are inline (no Mesh ctor/dtor referenced) so this stays GL-free.
    return BuildEditMesh(mesh.Vertices(), mesh.Indices());
}

const EditEdge* FindEdge(const EditMesh& mesh, uint32_t a, uint32_t b)
{
    if (a == b)
        return nullptr;
    uint32_t lo = std::min(a, b), hi = std::max(a, b);
    uint64_t k = ((uint64_t)lo << 32) | hi;
    auto it = mesh.edgeLookup.find(k);
    return it == mesh.edgeLookup.end() ? nullptr : &mesh.edges[it->second];
}

bool IsCreaseEdge(const EditEdge& edge, float angleThresholdRad)
{
    if (edge.kind != EdgeKind::Manifold)
        return true; // boundary + non-manifold edges are always real
    return edge.dihedral > angleThresholdRad;
}

float DistancePointSegment2D(const vec2& p, const vec2& a, const vec2& b)
{
    vec2 ab = b - a;
    float len2 = glm::dot(ab, ab);
    if (len2 <= 0.0f)
        return glm::length(p - a); // degenerate segment
    float t = glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f);
    return glm::length(p - (a + t * ab));
}

bool PointInTriangle2D(const vec2& p, const vec2& a, const vec2& b, const vec2& c)
{
    // Sign of the 2D cross product for each edge; p is inside when all three
    // agree (or are zero, i.e. on an edge). Winding-independent.
    auto edge = [](const vec2& u, const vec2& v, const vec2& q) {
        return (v.x - u.x) * (q.y - u.y) - (v.y - u.y) * (q.x - u.x);
    };
    float d0 = edge(a, b, p), d1 = edge(b, c, p), d2 = edge(c, a, p);
    bool neg = d0 < 0.0f || d1 < 0.0f || d2 < 0.0f;
    bool pos = d0 > 0.0f || d1 > 0.0f || d2 > 0.0f;
    return !(neg && pos);
}

bool PointInRect2D(const vec2& p, const vec2& mn, const vec2& mx)
{
    return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y;
}

std::vector<uint32_t> ResolveVertexSet(const EditMesh& mesh, ElementKind kind,
                                       const std::vector<uint32_t>& selected)
{
    std::vector<uint32_t> out;
    auto push = [&](uint32_t v) {
        if (std::find(out.begin(), out.end(), v) == out.end())
            out.push_back(v);
    };
    for (uint32_t id : selected) {
        if (kind == ElementKind::Vertex) {
            if (id < mesh.vertices.size())
                push(id);
        } else if (kind == ElementKind::Edge) {
            if (id < mesh.edges.size()) {
                push(mesh.edges[id].v0);
                push(mesh.edges[id].v1);
            }
        } else { // Face
            if (id < mesh.faces.size())
                for (uint32_t c = 0; c < 3; ++c)
                    push(mesh.faces[id].v[c]);
        }
    }
    return out;
}

vec3 SelectionCentroid(const EditMesh& mesh, const std::vector<uint32_t>& vertexIds)
{
    if (vertexIds.empty())
        return vec3(0.0f);
    vec3 acc(0.0f);
    for (uint32_t v : vertexIds)
        if (v < mesh.vertices.size())
            acc += mesh.vertices[v].position;
    return acc / (float)vertexIds.size();
}

void ApplyVertexTransform(std::vector<Vertex>& meshVertices, const EditMesh& mesh,
                          const std::vector<uint32_t>& vertexIds, const std::vector<vec3>& startPositions,
                          const mat4& xform)
{
    for (size_t i = 0; i < vertexIds.size() && i < startPositions.size(); ++i) {
        if (vertexIds[i] >= mesh.vertices.size())
            continue;
        vec3 p = vec3(xform * vec4(startPositions[i], 1.0f));
        for (uint32_t raw : mesh.vertices[vertexIds[i]].rawVerts)
            if (raw < meshVertices.size())
                meshVertices[raw].position = p;
    }
}

} // namespace forge
