#include "test_framework.h"

#include <forge/geometry/EditMesh.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <vector>

namespace forge::test {

namespace {

// Build a unit cube the way MeshFactory does: 6 faces, 4 per-face-duplicated
// corners each (24 verts), each face split into 2 triangles on the c0-c2
// diagonal. Only positions matter to EditMesh, so normals/uvs are left zero.
void AddQuad(std::vector<Vertex>& v, std::vector<uint32_t>& idx, vec3 a, vec3 b, vec3 c, vec3 d)
{
    uint32_t base = (uint32_t)v.size();
    v.push_back({a, vec3(0.0f), vec2(0.0f)});
    v.push_back({b, vec3(0.0f), vec2(0.0f)});
    v.push_back({c, vec3(0.0f), vec2(0.0f)});
    v.push_back({d, vec3(0.0f), vec2(0.0f)});
    idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

EditMesh BuildCube()
{
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    const float h = 0.5f;
    AddQuad(v, idx, {h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h});     // +X
    AddQuad(v, idx, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h}); // -X
    AddQuad(v, idx, {-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h});     // +Y
    AddQuad(v, idx, {-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}); // -Y
    AddQuad(v, idx, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h});     // +Z
    AddQuad(v, idx, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}); // -Z
    return BuildEditMesh(v, idx);
}

} // namespace

void RunEditMeshTests()
{
    // --- cube welds 24 raw verts into 8 group vertices ------------------------
    {
        EditMesh m = BuildCube();
        CHECK(m.vertices.size() == 8);
        CHECK(m.faces.size() == 12); // 6 quads x 2 tris
        for (const EditVertex& vert : m.vertices)
            CHECK(vert.rawVerts.size() == 3); // each corner shared by 3 faces
    }

    // --- 18 group-edges: 12 cube creases + 6 coplanar face diagonals ----------
    {
        EditMesh m = BuildCube();
        CHECK(m.edges.size() == 18);

        int boundary = 0, manifold = 0, nonManifold = 0, crease = 0, flat = 0;
        const float kThresh = glm::radians(30.0f);
        for (const EditEdge& e : m.edges) {
            if (e.kind == EdgeKind::Boundary) ++boundary;
            else if (e.kind == EdgeKind::Manifold) ++manifold;
            else ++nonManifold;
            if (IsCreaseEdge(e, kThresh)) ++crease;
            else ++flat;
        }
        CHECK(boundary == 0);     // closed surface — no open borders
        CHECK(manifold == 18);    // every edge shared by exactly 2 triangles
        CHECK(nonManifold == 0);
        CHECK(crease == 12);      // the real cube edges
        CHECK(flat == 6);         // the diagonal across each flat face is hidden
    }

    // --- dihedral angles: 90 deg at a cube edge, 0 across a flat face ----------
    {
        EditMesh m = BuildCube();
        int near90 = 0, near0 = 0;
        for (const EditEdge& e : m.edges) {
            if (ApproxEq(e.dihedral, glm::half_pi<float>(), 1e-3f)) ++near90;
            else if (ApproxEq(e.dihedral, 0.0f, 1e-3f)) ++near0;
        }
        CHECK(near90 == 12);
        CHECK(near0 == 6);
    }

    // --- adjacency round-trips ------------------------------------------------
    {
        EditMesh m = BuildCube();
        for (uint32_t f = 0; f < m.faces.size(); ++f) {
            for (uint32_t s = 0; s < 3; ++s) {
                uint32_t e = m.faces[f].edges[s];
                CHECK(e != kNoEdge); // no degenerate edges on a clean cube
                const std::vector<uint32_t>& ef = m.edges[e].faces;
                CHECK(std::find(ef.begin(), ef.end(), f) != ef.end());
            }
        }
        for (uint32_t e = 0; e < m.edges.size(); ++e) {
            for (uint32_t f : m.edges[e].faces) {
                const uint32_t* fe = m.faces[f].edges;
                CHECK(fe[0] == e || fe[1] == e || fe[2] == e);
            }
        }
    }

    // --- FindEdge is order-independent and rejects bad queries ----------------
    {
        EditMesh m = BuildCube();
        const EditEdge& e = m.edges[0];
        CHECK(FindEdge(m, e.v0, e.v1) == &e);
        CHECK(FindEdge(m, e.v1, e.v0) == &e);
        CHECK(FindEdge(m, 3, 3) == nullptr); // a == b
    }

    // --- an open triangle reports 3 boundary edges, all drawn as creases ------
    {
        std::vector<Vertex> v = {{{0.0f, 0.0f, 0.0f}, vec3(0.0f), vec2(0.0f)},
                                 {{1.0f, 0.0f, 0.0f}, vec3(0.0f), vec2(0.0f)},
                                 {{0.0f, 1.0f, 0.0f}, vec3(0.0f), vec2(0.0f)}};
        std::vector<uint32_t> idx = {0, 1, 2};
        EditMesh m = BuildEditMesh(v, idx);
        CHECK(m.vertices.size() == 3);
        CHECK(m.edges.size() == 3);
        for (const EditEdge& e : m.edges) {
            CHECK(e.kind == EdgeKind::Boundary);
            CHECK(IsCreaseEdge(e, glm::radians(30.0f))); // boundary always drawn
        }
    }
}

} // namespace forge::test
