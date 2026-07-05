#include "test_framework.h"

#include "mcp/McpElements.h"

#include <glm/gtc/matrix_transform.hpp>

#include <vector>

// Suites for the element-query kernel (#91): world-space centers/normals,
// radius filter, cap accounting, face->edge mapping — all GL-free.

namespace forge::test {

// Unit quad in the y=0 plane: 4 welded verts, 5 edges (4 border + diagonal),
// 2 faces. Winding {0,1,2} gives a -y geometric normal.
static void BuildQuad(std::vector<Vertex>& verts, std::vector<uint32_t>& indices)
{
    verts = {{{0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
             {{1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
             {{1.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
             {{0.0f, 0.0f, 1.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}}};
    indices = {0, 1, 2, 0, 2, 3};
}

static void FaceListing()
{
    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;
    BuildQuad(verts, indices);
    const EditMesh em = BuildEditMesh(verts, indices);
    CHECK(em.faces.size() == 2);
    CHECK(em.edges.size() == 5);

    const mat4 world = glm::translate(mat4(1.0f), {10.0f, 0.0f, 0.0f});
    size_t total = 0;
    auto faces = ListFaceElements(em, world, nullptr, 0.0f, 100, total);
    CHECK(total == 2 && faces.size() == 2);
    // Face 0 = tri {0,1,2}: centroid (2/3, 0, 1/3), translated +10 on x.
    CHECK(ApproxEq(faces[0].center.x, 10.0f + 2.0f / 3.0f, 1e-4f));
    CHECK(ApproxEq(faces[0].center.z, 1.0f / 3.0f, 1e-4f));
    CHECK(ApproxEq(faces[0].normal.y, -1.0f, 1e-4f)); // -y winding, translation-invariant

    // Non-uniform scale must renormalize the transformed normal.
    const mat4 scaled = glm::scale(world, {3.0f, 1.0f, 1.0f});
    faces = ListFaceElements(em, scaled, nullptr, 0.0f, 100, total);
    CHECK(ApproxEq(glm::length(faces[0].normal), 1.0f, 1e-4f));
    CHECK(ApproxEq(faces[0].normal.y, -1.0f, 1e-4f));

    // Radius filter: only face 0's centroid sits within 0.01 of it.
    const vec3 c0{10.0f + 2.0f / 3.0f, 0.0f, 1.0f / 3.0f};
    faces = ListFaceElements(em, world, &c0, 0.01f, 100, total);
    CHECK(total == 1 && faces.size() == 1 && faces[0].id == 0);

    // Cap: total counts every match even when the list is truncated.
    faces = ListFaceElements(em, world, nullptr, 0.0f, 1, total);
    CHECK(total == 2 && faces.size() == 1);
}

static void EdgeListing()
{
    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;
    BuildQuad(verts, indices);
    const EditMesh em = BuildEditMesh(verts, indices);

    const mat4 world = glm::translate(mat4(1.0f), {10.0f, 0.0f, 0.0f});
    size_t total = 0;
    auto edges = ListEdgeElements(em, world, nullptr, 0.0f, 100, total);
    CHECK(total == 5 && edges.size() == 5);
    for (const ElementInfo& el : edges) {
        CHECK(el.center.x >= 10.0f - 1e-4f && el.center.x <= 11.0f + 1e-4f);
        CHECK(glm::length(el.normal) == 0.0f); // edges carry no normal
    }

    // The bottom border edge (0,0,0)-(1,0,0) has midpoint (10.5, 0, 0).
    const vec3 mid{10.5f, 0.0f, 0.0f};
    edges = ListEdgeElements(em, world, &mid, 0.01f, 100, total);
    CHECK(total == 1 && edges.size() == 1);
}

static void FaceToEdges()
{
    std::vector<Vertex> verts;
    std::vector<uint32_t> indices;
    BuildQuad(verts, indices);
    const EditMesh em = BuildEditMesh(verts, indices);

    // Both faces together bound all 5 edges, the shared diagonal only once.
    auto all = EdgesOfFaces(em, {0, 1});
    CHECK(all.size() == 5);

    // A single triangle has exactly 3; stale ids are skipped, not fatal.
    CHECK(EdgesOfFaces(em, {0}).size() == 3);
    CHECK(EdgesOfFaces(em, {0, 99}).size() == 3);
    CHECK(EdgesOfFaces(em, {}).empty());
}

void RunMcpElementsTests()
{
    FaceListing();
    EdgeListing();
    FaceToEdges();
    std::printf("[ok] mcp element-query tests done\n");
}

} // namespace forge::test
