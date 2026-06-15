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

// Raw cube geometry: faces in order +X(tris 0,1) -X(2,3) +Y(4,5) -Y(6,7)
// +Z(8,9) -Z(10,11), each quad split on the c0-c2 diagonal.
void BuildCubeRaw(std::vector<Vertex>& v, std::vector<uint32_t>& idx)
{
    const float h = 0.5f;
    AddQuad(v, idx, {h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h});     // +X
    AddQuad(v, idx, {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h}); // -X
    AddQuad(v, idx, {-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h});     // +Y
    AddQuad(v, idx, {-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}); // -Y
    AddQuad(v, idx, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h});     // +Z
    AddQuad(v, idx, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}); // -Z
}

EditMesh BuildCube()
{
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    BuildCubeRaw(v, idx);
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
        // Stale ids are skipped in the divisor too: {0, bogus} == just vertex 0.
        vec3 v0 = m.vertices[0].position;
        vec3 mixed = SelectionCentroid(m, {0, 999999});
        CHECK(ApproxEq(mixed.x, v0.x) && ApproxEq(mixed.y, v0.y) && ApproxEq(mixed.z, v0.z));
        CHECK(ApproxEq(SelectionCentroid(m, {999999}).x, 0.0f)); // all stale -> origin
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

    // --- BuildFaceExtrusion: one cube face -> cap + 4 walls, diagonal has none -
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        BuildCubeRaw(verts, idx);
        EditMesh m = BuildEditMesh(verts, idx);

        // +X face = triangles 0,1 (the two halves of one quad).
        FaceExtrusion ex = BuildFaceExtrusion(m, verts, idx, {0, 1});
        CHECK(!ex.indices.empty());
        // Region normal points +X.
        CHECK(ApproxEq(ex.normal.x, 1.0f) && ApproxEq(ex.normal.y, 0.0f) && ApproxEq(ex.normal.z, 0.0f));
        // 4 cap corners + 2 wall tops x 4 boundary edges = 12 sliding verts.
        CHECK(ex.capVerts.size() == 12);
        // 24 source + 4 cap + 4 walls x 4 verts = 44 verts.
        CHECK(ex.vertices.size() == 44);
        // 36 source + 4 walls x 6 = 60 indices (20 tris). The shared quad diagonal
        // is an interior region edge, so it grows no wall (4 walls, not 5).
        CHECK(ex.indices.size() == 60);

        // Slide the cap out and confirm the re-pointed face triangles followed.
        const float off = 0.5f;
        for (uint32_t v : ex.capVerts)
            ex.vertices[v].position += ex.normal * off;
        for (int c = 0; c < 6; ++c) // tris 0,1 corners all sit on the raised cap
            CHECK(ApproxEq(ex.vertices[ex.indices[c]].position.x, 1.0f));
        // A floor corner (an original +X vert, now only walled) stayed put.
        CHECK(ApproxEq(verts[idx[0]].position.x, 0.5f));
    }

    // --- BuildFaceExtrusion: degenerate / empty selections return nothing ------
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        BuildCubeRaw(verts, idx);
        EditMesh m = BuildEditMesh(verts, idx);
        CHECK(BuildFaceExtrusion(m, verts, idx, {}).indices.empty());        // empty selection
        CHECK(BuildFaceExtrusion(m, verts, idx, {0, 2}).indices.empty());    // +X & -X normals cancel
        CHECK(BuildFaceExtrusion(m, verts, idx, {999}).indices.empty());     // stale face id
    }

    // --- BuildEdgeExtrusion: a quad's boundary edge -> one bridging quad -------
    {
        // Single quad in the XY plane (+Z normal), two tris on the 0-2 diagonal.
        std::vector<Vertex> verts = {{{0.0f, 0.0f, 0.0f}, vec3(0.0f), vec2(0.0f)},
                                     {{1.0f, 0.0f, 0.0f}, vec3(0.0f), vec2(0.0f)},
                                     {{1.0f, 1.0f, 0.0f}, vec3(0.0f), vec2(0.0f)},
                                     {{0.0f, 1.0f, 0.0f}, vec3(0.0f), vec2(0.0f)}};
        std::vector<uint32_t> idx = {0, 1, 2, 0, 2, 3};
        EditMesh m = BuildEditMesh(verts, idx);

        // Bottom edge g0-g1; it belongs to only tri0, so it's a boundary edge.
        const EditEdge* e01 = FindEdge(m, 0, 1);
        CHECK(e01 != nullptr);
        CHECK(e01->kind == EdgeKind::Boundary);
        uint32_t eid = (uint32_t)(e01 - m.edges.data());

        EdgeExtrusion ex = BuildEdgeExtrusion(m, verts, idx, {eid});
        CHECK(!ex.indices.empty());
        // In-plane, perpendicular to the edge, away from the quad interior: -Y.
        CHECK(ApproxEq(ex.normal.x, 0.0f) && ApproxEq(ex.normal.y, -1.0f) && ApproxEq(ex.normal.z, 0.0f));
        CHECK(ex.movingVerts.size() == 2);          // the new top edge
        CHECK(ex.vertices.size() == 4 + 4);         // 2 bottom + 2 top duplicates
        CHECK(ex.indices.size() == 6 + 6);          // one bridging quad (2 tris)
        CHECK(ex.newEdges.size() == 1);

        const float off = 0.5f;
        for (uint32_t v : ex.movingVerts)
            ex.vertices[v].position += ex.normal * off;
        for (uint32_t v : ex.movingVerts) // the pulled edge sits at y = -0.5
            CHECK(ApproxEq(ex.vertices[v].position.y, -0.5f));
    }

    // --- BuildEdgeExtrusion: connected boundary edges pull as one strip --------
    {
        std::vector<Vertex> verts = {{{0.0f, 0.0f, 0.0f}, vec3(0.0f), vec2(0.0f)},
                                     {{1.0f, 0.0f, 0.0f}, vec3(0.0f), vec2(0.0f)},
                                     {{1.0f, 1.0f, 0.0f}, vec3(0.0f), vec2(0.0f)},
                                     {{0.0f, 1.0f, 0.0f}, vec3(0.0f), vec2(0.0f)}};
        std::vector<uint32_t> idx = {0, 1, 2, 0, 2, 3};
        EditMesh m = BuildEditMesh(verts, idx);
        uint32_t e01 = (uint32_t)(FindEdge(m, 0, 1) - m.edges.data());
        uint32_t e12 = (uint32_t)(FindEdge(m, 1, 2) - m.edges.data());

        EdgeExtrusion ex = BuildEdgeExtrusion(m, verts, idx, {e01, e12});
        // Shared corner g1 dedups: 3 top verts (not 4), 3 bottoms, 2 quads.
        CHECK(ex.movingVerts.size() == 3);
        CHECK(ex.newEdges.size() == 2);
        CHECK(ex.indices.size() == 6 + 12); // two bridging quads
    }

    // --- BuildEdgeExtrusion: a cube (manifold) edge lifts a ridge --------------
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        BuildCubeRaw(verts, idx);
        EditMesh m = BuildEditMesh(verts, idx);
        auto groupAt = [&](vec3 p) {
            for (uint32_t g = 0; g < m.vertices.size(); ++g)
                if (ApproxEq(m.vertices[g].position.x, p.x) && ApproxEq(m.vertices[g].position.y, p.y) &&
                    ApproxEq(m.vertices[g].position.z, p.z))
                    return g;
            return UINT32_MAX;
        };
        // Vertical edge shared by the +X and +Y faces.
        uint32_t ga = groupAt({0.5f, 0.5f, 0.5f}), gb = groupAt({0.5f, 0.5f, -0.5f});
        CHECK(ga != UINT32_MAX && gb != UINT32_MAX);
        const EditEdge* edge = FindEdge(m, ga, gb);
        CHECK(edge != nullptr && edge->kind == EdgeKind::Manifold);
        uint32_t eid = (uint32_t)(edge - m.edges.data());

        EdgeExtrusion ex = BuildEdgeExtrusion(m, verts, idx, {eid});
        // Ridge: averaged +X and +Y normals -> diagonal (0.707, 0.707, 0).
        CHECK(ApproxEq(ex.normal.x, 0.70710678f) && ApproxEq(ex.normal.y, 0.70710678f) &&
              ApproxEq(ex.normal.z, 0.0f));
        CHECK(ex.movingVerts.size() == 2);
        CHECK(ex.indices.size() == 36 + 6); // one bridging quad on top of the cube
    }

    // --- BuildEdgeExtrusion: empty / stale selections return nothing -----------
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        BuildCubeRaw(verts, idx);
        EditMesh m = BuildEditMesh(verts, idx);
        CHECK(BuildEdgeExtrusion(m, verts, idx, {}).indices.empty());    // empty selection
        CHECK(BuildEdgeExtrusion(m, verts, idx, {999}).indices.empty()); // stale edge id
    }

    // Closed + manifold check shared by the subdivision tests: a watertight result
    // has no open borders and no edge shared by >2 faces (no T-junction crack).
    auto isWatertight = [](const EditMesh& r) {
        for (const EditEdge& e : r.edges)
            if (e.kind != EdgeKind::Manifold)
                return false;
        return true;
    };

    // --- BuildFaceSubdivision: one cube face -> 4 tris/quad, neighbours kept --
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        BuildCubeRaw(verts, idx);
        EditMesh m = BuildEditMesh(verts, idx);

        MeshSubdivision sd = BuildFaceSubdivision(m, verts, idx, {0, 1}); // the +X quad
        CHECK(!sd.indices.empty());
        CHECK(sd.newVerts.size() == 5);    // 4 outer edge midpoints + 1 shared diagonal
        CHECK(sd.indices.size() == 66);    // 2 faces x4 + 4 neighbour splits + 6 untouched = 22 tris

        // Every inserted midpoint lies on the +X plane; one sits at the face centre.
        bool haveCenter = false;
        for (uint32_t v : sd.newVerts) {
            CHECK(ApproxEq(sd.vertices[v].position.x, 0.5f));
            if (ApproxEq(sd.vertices[v].position.y, 0.0f) && ApproxEq(sd.vertices[v].position.z, 0.0f))
                haveCenter = true;
        }
        CHECK(haveCenter);

        // Rebuild: 8 cube groups + 5 midpoints; still a closed, manifold surface
        // (the neighbour splits removed the T-junctions).
        EditMesh r = BuildEditMesh(sd.vertices, sd.indices);
        CHECK(r.vertices.size() == 13);
        CHECK(r.faces.size() == 22);
        CHECK(isWatertight(r));
    }

    // --- BuildEdgeSubdivision: split one cube edge, both incident faces split --
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        BuildCubeRaw(verts, idx);
        EditMesh m = BuildEditMesh(verts, idx);
        auto groupAt = [&](vec3 p) {
            for (uint32_t g = 0; g < m.vertices.size(); ++g)
                if (ApproxEq(m.vertices[g].position.x, p.x) && ApproxEq(m.vertices[g].position.y, p.y) &&
                    ApproxEq(m.vertices[g].position.z, p.z))
                    return g;
            return UINT32_MAX;
        };
        uint32_t ga = groupAt({0.5f, 0.5f, 0.5f}), gb = groupAt({0.5f, 0.5f, -0.5f});
        const EditEdge* edge = FindEdge(m, ga, gb);
        CHECK(edge != nullptr && edge->kind == EdgeKind::Manifold);
        uint32_t eid = (uint32_t)(edge - m.edges.data());

        MeshSubdivision sd = BuildEdgeSubdivision(m, verts, idx, {eid});
        CHECK(sd.newVerts.size() == 1);
        CHECK(sd.indices.size() == 42); // 2 incident tris -> 2 each, 10 untouched = 14 tris
        CHECK(ApproxEq(sd.vertices[sd.newVerts[0]].position.x, 0.5f) &&
              ApproxEq(sd.vertices[sd.newVerts[0]].position.y, 0.5f) &&
              ApproxEq(sd.vertices[sd.newVerts[0]].position.z, 0.0f)); // edge midpoint

        EditMesh r = BuildEditMesh(sd.vertices, sd.indices);
        CHECK(r.vertices.size() == 9); // 8 + 1 midpoint
        CHECK(r.faces.size() == 14);
        CHECK(isWatertight(r));
    }

    // --- BuildFace/EdgeSubdivision: empty / stale selections return nothing ----
    {
        std::vector<Vertex> verts;
        std::vector<uint32_t> idx;
        BuildCubeRaw(verts, idx);
        EditMesh m = BuildEditMesh(verts, idx);
        CHECK(BuildFaceSubdivision(m, verts, idx, {}).indices.empty());
        CHECK(BuildFaceSubdivision(m, verts, idx, {999}).indices.empty());
        CHECK(BuildEdgeSubdivision(m, verts, idx, {}).indices.empty());
        CHECK(BuildEdgeSubdivision(m, verts, idx, {999}).indices.empty());
    }
}

} // namespace forge::test
