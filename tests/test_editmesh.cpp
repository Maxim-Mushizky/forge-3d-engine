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

    // --- DistancePointSegment2D: projection, clamping, degenerate -------------
    {
        CHECK(ApproxEq(DistancePointSegment2D({5.0f, 3.0f}, {0.0f, 0.0f}, {10.0f, 0.0f}), 3.0f)); // foot inside
        CHECK(ApproxEq(DistancePointSegment2D({-4.0f, 0.0f}, {0.0f, 0.0f}, {10.0f, 0.0f}), 4.0f)); // past 'a'
        CHECK(ApproxEq(DistancePointSegment2D({13.0f, 4.0f}, {0.0f, 0.0f}, {10.0f, 0.0f}), 5.0f)); // past 'b' (3-4-5)
        CHECK(ApproxEq(DistancePointSegment2D({3.0f, 4.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}), 5.0f));    // degenerate
    }

    // --- PointInTriangle2D: inside / outside / on-edge, both windings ---------
    {
        vec2 a{0.0f, 0.0f}, b{4.0f, 0.0f}, c{0.0f, 4.0f};
        CHECK(PointInTriangle2D({1.0f, 1.0f}, a, b, c));      // inside
        CHECK(!PointInTriangle2D({3.0f, 3.0f}, a, b, c));     // outside (beyond hypotenuse)
        CHECK(PointInTriangle2D({2.0f, 0.0f}, a, b, c));      // on an edge
        CHECK(PointInTriangle2D({1.0f, 1.0f}, a, c, b));      // reversed winding, same result
        CHECK(!PointInTriangle2D({-1.0f, 1.0f}, a, b, c));    // outside
    }

    // --- PointInRect2D --------------------------------------------------------
    {
        vec2 mn{1.0f, 2.0f}, mx{5.0f, 6.0f};
        CHECK(PointInRect2D({3.0f, 4.0f}, mn, mx));
        CHECK(PointInRect2D({1.0f, 2.0f}, mn, mx)); // corner inclusive
        CHECK(!PointInRect2D({0.0f, 4.0f}, mn, mx));
        CHECK(!PointInRect2D({3.0f, 7.0f}, mn, mx));
    }

    // --- ResolveVertexSet: per-kind expansion + dedup -------------------------
    {
        EditMesh m = BuildCube();
        // Vertex: ids pass through, deduped.
        auto vs = ResolveVertexSet(m, ElementKind::Vertex, {2, 2, 5});
        CHECK(vs.size() == 2);
        // Edge: two endpoints; a shared vertex across two edges dedups.
        const EditEdge& e0 = m.edges[0];
        auto es = ResolveVertexSet(m, ElementKind::Edge, {0});
        CHECK(es.size() == 2);
        CHECK((es[0] == e0.v0 || es[1] == e0.v0));
        // Face: three corners.
        auto fs = ResolveVertexSet(m, ElementKind::Face, {0});
        CHECK(fs.size() == 3);
        // All faces together cover all 8 cube vertices.
        std::vector<uint32_t> allFaces(m.faces.size());
        for (uint32_t i = 0; i < m.faces.size(); ++i) allFaces[i] = i;
        CHECK(ResolveVertexSet(m, ElementKind::Face, allFaces).size() == 8);
    }

    // --- SelectionCentroid ----------------------------------------------------
    {
        EditMesh m = BuildCube();
        std::vector<uint32_t> all(m.vertices.size());
        for (uint32_t i = 0; i < m.vertices.size(); ++i) all[i] = i;
        vec3 c = SelectionCentroid(m, all);
        CHECK(ApproxEq(c.x, 0.0f) && ApproxEq(c.y, 0.0f) && ApproxEq(c.z, 0.0f)); // unit cube centered
        CHECK(ApproxEq(SelectionCentroid(m, {}).x, 0.0f)); // empty -> origin
    }

    // --- ApplyVertexTransform: writes all rawVerts of a group, leaves rest ----
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        const float h = 0.5f;
        // reuse the cube builder's geometry by rebuilding raw data here
        auto quad = [&](vec3 a, vec3 b, vec3 c, vec3 d) {
            uint32_t base = (uint32_t)verts.size();
            verts.push_back({a, vec3(0.0f), vec2(0.0f)});
            verts.push_back({b, vec3(0.0f), vec2(0.0f)});
            verts.push_back({c, vec3(0.0f), vec2(0.0f)});
            verts.push_back({d, vec3(0.0f), vec2(0.0f)});
            idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        };
        quad({h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h});
        quad({-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h});
        quad({-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h});
        quad({-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h});
        quad({-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h});
        quad({h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h});
        EditMesh m = BuildEditMesh(verts, idx);

        uint32_t v = 0;                         // move one welded corner group
        vec3 start = m.vertices[v].position;
        size_t raws = m.vertices[v].rawVerts.size();
        CHECK(raws == 3);                       // cube corner shared by 3 faces
        mat4 shift = glm::translate(mat4(1.0f), vec3(10.0f, 0.0f, 0.0f));
        ApplyVertexTransform(verts, m, {v}, {start}, shift);
        for (uint32_t raw : m.vertices[v].rawVerts)
            CHECK(ApproxEq(verts[raw].position.x, start.x + 10.0f));
        // a vertex NOT in the set is untouched.
        uint32_t other = (v == 1) ? 2 : 1;
        for (uint32_t raw : m.vertices[other].rawVerts)
            CHECK(ApproxEq(verts[raw].position.x, m.vertices[other].position.x));
    }
}

} // namespace forge::test
