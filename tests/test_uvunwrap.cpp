#include "test_framework.h"

#include "forge/geometry/UvUnwrap.h"

#include <cmath>
#include <vector>

// UV unwrap kernel (#81): xatlas atlas generation. Checks the contract the
// engine relies on — normalized in-range UVs, preserved triangle order (so
// submesh ranges stay valid), original attributes carried through xref, and a
// sane atlas (chart count, area coverage) for a plain cube.

namespace forge::test {
namespace {

// 24-vertex cube like MeshFactory builds: per-face corners, every face mapping
// the full [0,1] square (deliberately overlapping — unwrap must fix that).
void BuildCube(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    const vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        vec3 n = normals[f];
        vec3 t = std::abs(n.y) > 0.5f ? vec3(1, 0, 0) : vec3(0, 1, 0);
        vec3 b = glm::cross(n, t);
        uint32_t base = (uint32_t)vertices.size();
        const vec2 corners[4] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
        for (int c = 0; c < 4; ++c) {
            vec3 pos = n * 0.5f + t * corners[c].x + b * corners[c].y;
            vertices.push_back({pos, n, corners[c] + vec2(0.5f)});
        }
        indices.insert(indices.end(),
                       {base, base + 1, base + 2, base, base + 2, base + 3});
    }
}

bool FiniteUnit(const vec2& uv)
{
    return std::isfinite(uv.x) && std::isfinite(uv.y) && uv.x >= 0.0f && uv.x <= 1.0f &&
           uv.y >= 0.0f && uv.y <= 1.0f;
}

void TestCubeUnwrap()
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    BuildCube(vertices, indices);

    auto result = UnwrapUVData(vertices, indices, {});
    CHECK(result.has_value());
    if (!result)
        return;

    // Same triangles, possibly more vertices (seam splits), never fewer.
    CHECK(result->indices.size() == indices.size());
    CHECK(result->vertices.size() >= 8);
    CHECK(result->atlasWidth > 0 && result->atlasHeight > 0);

    // Every UV normalized, finite, inside the atlas.
    for (const Vertex& v : result->vertices)
        CHECK(FiniteUnit(v.uv));

    // Sane chart count for a cube: ~1 per face, allow headroom for splits but
    // reject a chart-per-triangle explosion.
    CHECK(result->chartCount >= 1 && result->chartCount <= 12);

    // Triangle order preserved AND attributes carried via xref: triangle i of
    // the output must have the same corner positions/normals as input tri i.
    // Submesh-range validity after unwrap depends on exactly this.
    for (size_t i = 0; i < indices.size(); ++i) {
        const Vertex& in = vertices[indices[i]];
        const Vertex& out = result->vertices[result->indices[i]];
        CHECK(ApproxEq(in.position.x, out.position.x) && ApproxEq(in.position.y, out.position.y) &&
              ApproxEq(in.position.z, out.position.z));
        CHECK(ApproxEq(in.normal.x, out.normal.x) && ApproxEq(in.normal.y, out.normal.y) &&
              ApproxEq(in.normal.z, out.normal.z));
    }

    // The input overlaps 6 faces onto one square (coverage ~6); the atlas must
    // be non-overlapping (<= 1) and not degenerate-tiny.
    float coverage = UvAreaCoverage(result->vertices, result->indices);
    CHECK(coverage > 0.3f && coverage <= 1.01f);
    CHECK(result->utilization > 0.3f && result->utilization <= 1.0f);
}

void TestInputCoverageReportsOverlap()
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    BuildCube(vertices, indices);
    // Six faces each covering the full unit square.
    float coverage = UvAreaCoverage(vertices, indices);
    CHECK(coverage > 5.9f && coverage < 6.1f);
}

void TestSubmeshRangesSurvive()
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    BuildCube(vertices, indices);
    // First 3 faces slot 0, last 3 faces slot 1 — a partition like the
    // multi-material importer emits (#80).
    std::vector<Submesh> submeshes = {{0, 18, 0}, {18, 18, 1}};

    auto result = UnwrapUVData(vertices, indices, submeshes);
    CHECK(result.has_value());
    if (!result)
        return;
    CHECK(result->submeshes.size() == 2);
    CHECK(result->submeshes[0].firstIndex == 0 && result->submeshes[0].indexCount == 18 &&
          result->submeshes[0].materialSlot == 0);
    CHECK(result->submeshes[1].firstIndex == 18 && result->submeshes[1].indexCount == 18 &&
          result->submeshes[1].materialSlot == 1);
    // The ranges must still index valid vertices.
    for (uint32_t idx : result->indices)
        CHECK(idx < result->vertices.size());
}

void TestMaterialSlotsSplitCharts()
{
    // One flat two-quad strip: without material ids xatlas charts the whole
    // plane as a single region, so a >= 2 chart count with two slots proves
    // faceMaterialData actually keeps charts inside their material slot.
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    auto quad = [&](float x0, float x1) {
        uint32_t base = (uint32_t)vertices.size();
        vertices.push_back({{x0, 0, 0}, {0, 0, 1}, {0, 0}});
        vertices.push_back({{x1, 0, 0}, {0, 0, 1}, {0, 0}});
        vertices.push_back({{x1, 1, 0}, {0, 0, 1}, {0, 0}});
        vertices.push_back({{x0, 1, 0}, {0, 0, 1}, {0, 0}});
        indices.insert(indices.end(),
                       {base, base + 1, base + 2, base, base + 2, base + 3});
    };
    quad(0.0f, 1.0f);
    quad(1.0f, 2.0f); // shares the x=1 edge positions -> colocal weld

    auto plain = UnwrapUVData(vertices, indices, {});
    CHECK(plain.has_value() && plain->chartCount == 1);

    std::vector<Submesh> submeshes = {{0, 6, 0}, {6, 6, 1}};
    auto split = UnwrapUVData(vertices, indices, submeshes);
    CHECK(split.has_value());
    if (!split)
        return;
    CHECK(split->chartCount >= 2);
    // Triangle order must hold with faceMaterialData active too.
    for (size_t i = 0; i < indices.size(); ++i) {
        const Vertex& in = vertices[indices[i]];
        const Vertex& out = split->vertices[split->indices[i]];
        CHECK(ApproxEq(in.position.x, out.position.x) && ApproxEq(in.position.y, out.position.y) &&
              ApproxEq(in.position.z, out.position.z));
    }
}

void TestHostileSubmeshesDegradeSafely()
{
    // Ranges that don't fit the index buffer must sanitize away (single-
    // material fallback), not index faceMaterials out of bounds.
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    BuildCube(vertices, indices);
    std::vector<Submesh> hostile = {{100, 30, 0}, {0, 0, 1}};
    auto result = UnwrapUVData(vertices, indices, hostile);
    CHECK(result.has_value());
    if (result)
        CHECK(result->submeshes.empty());
}

void TestUnwrapIsRepeatable()
{
    // Acceptance: unwrapping an already-unwrapped mesh replaces the atlas
    // cleanly — same triangle count, still-valid normalized UVs.
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    BuildCube(vertices, indices);

    auto first = UnwrapUVData(vertices, indices, {});
    CHECK(first.has_value());
    if (!first)
        return;
    auto second = UnwrapUVData(first->vertices, first->indices, {});
    CHECK(second.has_value());
    if (!second)
        return;
    CHECK(second->indices.size() == indices.size());
    for (const Vertex& v : second->vertices)
        CHECK(FiniteUnit(v.uv));
}

void TestEmptyAndInvalidInput()
{
    std::vector<Vertex> none;
    std::vector<uint32_t> noIdx;
    CHECK(!UnwrapUVData(none, noIdx, {}).has_value());

    // Non-multiple-of-3 index count is structurally invalid.
    std::vector<Vertex> tri = {{{0, 0, 0}, {0, 0, 1}, {0, 0}},
                               {{1, 0, 0}, {0, 0, 1}, {0, 0}},
                               {{0, 1, 0}, {0, 0, 1}, {0, 0}}};
    std::vector<uint32_t> bad = {0, 1};
    CHECK(!UnwrapUVData(tri, bad, {}).has_value());
}

void TestFullyDegenerateMeshFails()
{
    // Every triangle zero-area: xatlas ignores all faces, nothing chartable.
    std::vector<Vertex> vertices = {{{0, 0, 0}, {0, 0, 1}, {0, 0}},
                                    {{0, 0, 0}, {0, 0, 1}, {0, 0}},
                                    {{0, 0, 0}, {0, 0, 1}, {0, 0}}};
    std::vector<uint32_t> indices = {0, 1, 2};
    auto result = UnwrapUVData(vertices, indices, {});
    // Either a clean failure or a result with all-zero UVs is acceptable —
    // what must not happen is a crash or NaN output.
    if (result)
        for (const Vertex& v : result->vertices)
            CHECK(FiniteUnit(v.uv));
}

} // namespace

void RunUvUnwrapTests()
{
    TestCubeUnwrap();
    TestInputCoverageReportsOverlap();
    TestSubmeshRangesSurvive();
    TestMaterialSlotsSplitCharts();
    TestHostileSubmeshesDegradeSafely();
    TestUnwrapIsRepeatable();
    TestEmptyAndInvalidInput();
    TestFullyDegenerateMeshFails();
}

} // namespace forge::test
