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
    uint32_t valid = 0; // skip stale ids in the divisor too, or the mean pulls toward origin
    for (uint32_t v : vertexIds)
        if (v < mesh.vertices.size()) {
            acc += mesh.vertices[v].position;
            ++valid;
        }
    return valid ? acc / (float)valid : vec3(0.0f);
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

FaceExtrusion BuildFaceExtrusion(const EditMesh& mesh, const std::vector<Vertex>& srcVertices,
                                 const std::vector<uint32_t>& srcIndices,
                                 const std::vector<uint32_t>& selectedFaces)
{
    FaceExtrusion out;
    if (selectedFaces.empty())
        return out;
    uint32_t triCount = (uint32_t)(srcIndices.size() / 3);

    // Region direction: area-weighted sum of the selected faces' raw normals
    // (the unnormalized cross product is already proportional to area). A flat,
    // connected selection averages to its plane normal; opposite faces cancel.
    vec3 nSum(0.0f);
    for (uint32_t f : selectedFaces) {
        if (f >= triCount || f >= mesh.faces.size())
            return {}; // stale selection id
        const vec3& p0 = srcVertices[srcIndices[f * 3]].position;
        const vec3& p1 = srcVertices[srcIndices[f * 3 + 1]].position;
        const vec3& p2 = srcVertices[srcIndices[f * 3 + 2]].position;
        nSum += glm::cross(p1 - p0, p2 - p0);
    }
    float nLen = glm::length(nSum);
    if (nLen < 1e-12f)
        return {};
    out.normal = nSum / nLen;

    // Boundary edges in group space: an edge on exactly one selected face walls;
    // a shared interior edge (count 2) does not, so the region caps as one piece.
    std::unordered_map<uint64_t, int> regionEdgeCount;
    auto edgeKey = [](uint32_t ga, uint32_t gb) {
        if (ga > gb)
            std::swap(ga, gb);
        return ((uint64_t)ga << 32) | gb;
    };
    for (uint32_t f : selectedFaces) {
        const uint32_t* gv = mesh.faces[f].v; // EditVertex (weld-group) ids
        for (int c = 0; c < 3; ++c)
            ++regionEdgeCount[edgeKey(gv[c], gv[(c + 1) % 3])];
    }

    out.vertices = srcVertices;
    out.indices = srcIndices;

    // Cap: duplicate the region's raw verts and re-point the selected triangles
    // to them. The originals stay put as the floor (now referenced only by walls).
    std::unordered_map<uint32_t, uint32_t> capOf;
    auto capVert = [&](uint32_t raw) {
        auto [it, inserted] = capOf.try_emplace(raw, (uint32_t)out.vertices.size());
        if (inserted) {
            out.vertices.push_back(srcVertices[raw]);
            out.capVerts.push_back(it->second);
        }
        return it->second;
    };
    for (uint32_t f : selectedFaces)
        for (int c = 0; c < 3; ++c)
            out.indices[f * 3 + c] = capVert(srcIndices[f * 3 + c]);

    // Walls: a fresh quad per boundary edge — bottom shares the floor position,
    // top slides with the cap, so the rim reads as a hard edge by construction.
    for (uint32_t f : selectedFaces) {
        const uint32_t* gv = mesh.faces[f].v;
        for (int c = 0; c < 3; ++c) {
            if (regionEdgeCount[edgeKey(gv[c], gv[(c + 1) % 3])] != 1)
                continue; // interior region edge: no wall
            uint32_t a = srcIndices[f * 3 + c], b = srcIndices[f * 3 + (c + 1) % 3];
            uint32_t bottomA = (uint32_t)out.vertices.size();
            out.vertices.push_back(srcVertices[a]);
            uint32_t bottomB = (uint32_t)out.vertices.size();
            out.vertices.push_back(srcVertices[b]);
            uint32_t topA = (uint32_t)out.vertices.size();
            out.vertices.push_back(srcVertices[a]);
            uint32_t topB = (uint32_t)out.vertices.size();
            out.vertices.push_back(srcVertices[b]);
            out.capVerts.push_back(topA);
            out.capVerts.push_back(topB);
            // Matches the source edge's winding (region on the left) so the wall
            // faces outward for a positive offset; a negative offset flips it
            // into the cavity automatically since the top positions carry the sign.
            out.indices.insert(out.indices.end(), {bottomA, bottomB, topB, bottomA, topB, topA});
        }
    }
    return out;
}

EdgeExtrusion BuildEdgeExtrusion(const EditMesh& mesh, const std::vector<Vertex>& srcVertices,
                                 const std::vector<uint32_t>& srcIndices,
                                 const std::vector<uint32_t>& selectedEdges)
{
    EdgeExtrusion out;
    (void)srcIndices; // edges build from EditMesh group reps, not raw triangles
    if (selectedEdges.empty())
        return out;

    // Per-edge slide direction, summed: a boundary edge extends in its face's
    // plane (perpendicular to the edge, away from the face interior); a manifold
    // edge lifts along the two faces' averaged normal.
    vec3 dirSum(0.0f);
    for (uint32_t id : selectedEdges) {
        if (id >= mesh.edges.size())
            return {}; // stale selection id
        const EditEdge& E = mesh.edges[id];
        vec3 edgeVec = mesh.vertices[E.v1].position - mesh.vertices[E.v0].position;
        if (E.faces.size() == 1) {
            const EditFace& f = mesh.faces[E.faces[0]];
            vec3 perp = glm::cross(f.normal, edgeVec);
            if (glm::length(perp) < 1e-12f)
                continue;
            perp = glm::normalize(perp);
            // Flip so it points away from the face's third (off-edge) corner.
            uint32_t opp = f.v[0];
            for (uint32_t c = 0; c < 3; ++c)
                if (f.v[c] != E.v0 && f.v[c] != E.v1)
                    opp = f.v[c];
            vec3 mid = 0.5f * (mesh.vertices[E.v0].position + mesh.vertices[E.v1].position);
            if (glm::dot(perp, mesh.vertices[opp].position - mid) > 0.0f)
                perp = -perp;
            dirSum += perp;
        } else if (E.faces.size() >= 2) {
            dirSum += glm::normalize(mesh.faces[E.faces[0]].normal + mesh.faces[E.faces[1]].normal);
        }
        // A floating edge (no faces) has no orientation — it contributes nothing.
    }
    float dlen = glm::length(dirSum);
    if (dlen < 1e-12f)
        return {}; // no orientable edge, or directions cancelled
    out.normal = dirSum / dlen;

    out.vertices = srcVertices;
    out.indices = srcIndices;

    // One duplicated vertex per touched group, deduped so a connected selection
    // shares verts and pulls as a strip. Bottom verts sit on the source edge
    // (welded back to the surface by position); top verts slide.
    std::unordered_map<uint32_t, uint32_t> botOf, topOf;
    auto dupVert = [&](std::unordered_map<uint32_t, uint32_t>& cache, uint32_t group, bool moving) {
        auto [it, inserted] = cache.try_emplace(group, (uint32_t)out.vertices.size());
        if (inserted) {
            // Copy attributes from a representative raw vert, then pin the group rep.
            Vertex v = srcVertices[mesh.vertices[group].rawVerts[0]];
            v.position = mesh.vertices[group].position;
            out.vertices.push_back(v);
            if (moving)
                out.movingVerts.push_back(it->second);
        }
        return it->second;
    };

    for (uint32_t id : selectedEdges) {
        const EditEdge& E = mesh.edges[id];
        // Direct the edge as its incident face winds (region on a consistent side)
        // so the bridging quad faces outward like the rest of the surface.
        uint32_t a = E.v0, b = E.v1;
        if (!E.faces.empty()) {
            const EditFace& f = mesh.faces[E.faces[0]];
            for (uint32_t c = 0; c < 3; ++c) {
                uint32_t u = f.v[c], w = f.v[(c + 1) % 3];
                if ((u == E.v0 && w == E.v1) || (u == E.v1 && w == E.v0)) {
                    a = u;
                    b = w;
                    break;
                }
            }
        }
        uint32_t botA = dupVert(botOf, a, false), botB = dupVert(botOf, b, false);
        uint32_t topA = dupVert(topOf, a, true), topB = dupVert(topOf, b, true);
        out.newEdges.emplace_back(topA, topB);
        // Same winding pattern as the face-extrude walls; sign of the offset
        // carries the orientation, so push and pull both stay consistent.
        out.indices.insert(out.indices.end(), {botA, botB, topB, botA, topB, topA});
    }
    return out;
}

namespace {

uint64_t GroupEdgeKey(uint32_t ga, uint32_t gb)
{
    if (ga > gb)
        std::swap(ga, gb);
    return ((uint64_t)ga << 32) | gb;
}

// Re-triangulate one source triangle (a,b,c) given the midpoint vertex on each
// edge (kNoEdge = that edge isn't split). Red-green refinement: 3 splits -> the
// classic 1->4, 1 split -> bisect to the opposite corner, 2 splits -> a tip
// triangle plus a split quad. Windings follow the source (a->b->c).
void EmitSplitTriangle(std::vector<uint32_t>& out, uint32_t a, uint32_t b, uint32_t c,
                       uint32_t mAB, uint32_t mBC, uint32_t mCA)
{
    int splits = (mAB != kNoEdge) + (mBC != kNoEdge) + (mCA != kNoEdge);
    if (splits == 0) {
        out.insert(out.end(), {a, b, c});
    } else if (splits == 3) {
        out.insert(out.end(), {a, mAB, mCA, mAB, b, mBC, mCA, mBC, c, mAB, mBC, mCA});
    } else if (splits == 1) {
        if (mAB != kNoEdge)
            out.insert(out.end(), {a, mAB, c, mAB, b, c});
        else if (mBC != kNoEdge)
            out.insert(out.end(), {a, b, mBC, a, mBC, c});
        else // mCA
            out.insert(out.end(), {a, b, mCA, b, c, mCA});
    } else { // 2 splits: a tip triangle at the shared corner + the split quad
        if (mAB != kNoEdge && mBC != kNoEdge) // share b
            out.insert(out.end(), {mAB, b, mBC, a, mAB, mBC, a, mBC, c});
        else if (mBC != kNoEdge && mCA != kNoEdge) // share c
            out.insert(out.end(), {mBC, c, mCA, a, b, mCA, b, mBC, mCA});
        else // mCA && mAB, share a
            out.insert(out.end(), {a, mAB, mCA, mAB, b, c, mAB, c, mCA});
    }
}

// Shared core: split every group-edge in `splitEdges` at its midpoint and
// re-triangulate every affected triangle. Midpoints are deduped by group-edge
// key so the two faces across an edge share one vertex (watertight).
MeshSubdivision SubdivideByEdgeSet(const EditMesh& mesh, const std::vector<Vertex>& srcVertices,
                                   const std::vector<uint32_t>& srcIndices,
                                   const std::unordered_map<uint64_t, int>& splitEdges)
{
    MeshSubdivision out;
    if (splitEdges.empty())
        return out;
    out.vertices = srcVertices;

    // One midpoint vertex per split group-edge: position from the group reps,
    // attributes averaged from a representative raw vert of each endpoint. The
    // welded-normal recompute on commit fixes shading, so an approximate normal
    // here is fine (mirrors LoopSubdivide).
    std::unordered_map<uint64_t, uint32_t> midOf;
    for (const auto& [key, count] : splitEdges) {
        (void)count;
        uint32_t ga = (uint32_t)(key >> 32), gb = (uint32_t)(key & 0xFFFFFFFFu);
        const EditVertex& va = mesh.vertices[ga];
        const EditVertex& vb = mesh.vertices[gb];
        Vertex m = srcVertices[va.rawVerts[0]];
        m.position = 0.5f * (va.position + vb.position);
        m.uv = 0.5f * (srcVertices[va.rawVerts[0]].uv + srcVertices[vb.rawVerts[0]].uv);
        vec3 n = srcVertices[va.rawVerts[0]].normal + srcVertices[vb.rawVerts[0]].normal;
        float nl = glm::length(n);
        m.normal = nl > 1e-8f ? n / nl : vec3(0.0f, 1.0f, 0.0f);
        midOf[key] = (uint32_t)out.vertices.size();
        out.newVerts.push_back(midOf[key]);
        out.vertices.push_back(m);
    }

    out.indices.reserve(srcIndices.size());
    uint32_t triCount = (uint32_t)(srcIndices.size() / 3);
    auto midpoint = [&](uint32_t g0, uint32_t g1) {
        auto it = midOf.find(GroupEdgeKey(g0, g1));
        return it == midOf.end() ? kNoEdge : it->second;
    };
    for (uint32_t t = 0; t < triCount; ++t) {
        uint32_t a = srcIndices[t * 3], b = srcIndices[t * 3 + 1], c = srcIndices[t * 3 + 2];
        const uint32_t* gv = mesh.faces[t].v; // group ids of this triangle's corners
        EmitSplitTriangle(out.indices, a, b, c, midpoint(gv[0], gv[1]), midpoint(gv[1], gv[2]),
                          midpoint(gv[2], gv[0]));
    }
    return out;
}

} // namespace

MeshSubdivision BuildFaceSubdivision(const EditMesh& mesh, const std::vector<Vertex>& srcVertices,
                                     const std::vector<uint32_t>& srcIndices,
                                     const std::vector<uint32_t>& selectedFaces)
{
    uint32_t triCount = (uint32_t)(srcIndices.size() / 3);
    std::unordered_map<uint64_t, int> splitEdges;
    for (uint32_t f : selectedFaces) {
        if (f >= triCount || f >= mesh.faces.size())
            return {}; // stale selection id
        const uint32_t* gv = mesh.faces[f].v;
        for (int c = 0; c < 3; ++c)
            splitEdges[GroupEdgeKey(gv[c], gv[(c + 1) % 3])] = 1;
    }
    return SubdivideByEdgeSet(mesh, srcVertices, srcIndices, splitEdges);
}

MeshSubdivision BuildEdgeSubdivision(const EditMesh& mesh, const std::vector<Vertex>& srcVertices,
                                     const std::vector<uint32_t>& srcIndices,
                                     const std::vector<uint32_t>& selectedEdges)
{
    std::unordered_map<uint64_t, int> splitEdges;
    for (uint32_t id : selectedEdges) {
        if (id >= mesh.edges.size())
            return {}; // stale selection id
        const EditEdge& e = mesh.edges[id];
        splitEdges[GroupEdgeKey(e.v0, e.v1)] = 1;
    }
    return SubdivideByEdgeSet(mesh, srcVertices, srcIndices, splitEdges);
}

} // namespace forge
