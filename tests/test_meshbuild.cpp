#include "test_framework.h"

#include "forge/assets/MeshBuild.h"
#include "forge/geometry/MeshStats.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

// Lathe & sweep mesh-generation kernels (#111): watertightness for closed
// profiles, outward orientation, profile fidelity, seam welding, degenerate
// rejection. Everything runs on the raw MeshData buffers — no GL.

namespace forge::test {
namespace {

// Signed volume via the divergence theorem: positive when every triangle winds
// counter-clockwise seen from outside. The single strongest orientation check
// a closed mesh has (#116 taught us not to trust it implicitly).
float SignedVolume(const MeshData& m)
{
    float v6 = 0.0f;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const vec3& a = m.vertices[m.indices[i]].position;
        const vec3& b = m.vertices[m.indices[i + 1]].position;
        const vec3& c = m.vertices[m.indices[i + 2]].position;
        v6 += glm::dot(a, glm::cross(b, c));
    }
    return v6 / 6.0f;
}

MeshStats Stats(const MeshData& m)
{
    return ComputeMeshStats(m.vertices, m.indices);
}

bool SamePositionBits(const Vertex& a, const Vertex& b)
{
    return std::memcmp(&a.position, &b.position, sizeof(vec3)) == 0;
}

// One NaN anywhere in the output poisons AABBs, raycasts and exports — every
// build that returns true must be finite throughout.
bool AllFinite(const MeshData& m)
{
    for (const Vertex& v : m.vertices)
        for (float f : {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y,
                        v.normal.z})
            if (!std::isfinite(f))
                return false;
    return true;
}

// --- lathe -----------------------------------------------------------------

void TestLatheCylinderProfile()
{
    // Straight wall + caps must reproduce a cylinder exactly.
    const std::vector<vec2> profile = {{0.5f, -0.5f}, {0.5f, 0.5f}};
    const uint32_t sectors = 8;
    MeshData m;
    CHECK(BuildLathe(profile, sectors, true, m));
    // 2 wall rings of sectors+1, plus 2 caps of center + sectors+1.
    CHECK(m.vertices.size() == 2 * (sectors + 1) + 2 * (sectors + 2));
    MeshStats s = Stats(m);
    CHECK(s.watertight);
    CHECK(s.boundaryEdges == 0);
    CHECK(s.nonManifoldEdges == 0);
    CHECK(s.degenerateTriangles == 0);
    // Octagonal prism volume: base area 0.5 * n * r^2 * sin(2pi/n), height 1.
    const float base = 0.5f * sectors * 0.25f * std::sin(2.0f * 3.14159265f / sectors);
    CHECK(SignedVolume(m) > 0.0f);
    CHECK(ApproxEq(SignedVolume(m), base, 1e-3f));
    // Wall normals are radial and outward.
    for (uint32_t s2 = 0; s2 <= sectors; ++s2) {
        const Vertex& v = m.vertices[s2];
        const vec3 radial = glm::normalize(vec3(v.position.x, 0.0f, v.position.z));
        CHECK(ApproxEq(glm::dot(v.normal, radial), 1.0f, 1e-4f));
    }
}

void TestLathePoleClosedProfileIsWatertight()
{
    // Vase whose profile starts and ends on the axis: sealed by pole collapse
    // alone, no caps involved.
    const std::vector<vec2> profile = {
        {0.0f, 0.0f}, {0.4f, 0.1f}, {0.5f, 0.5f}, {0.3f, 0.9f}, {0.0f, 1.0f}};
    MeshData m;
    CHECK(BuildLathe(profile, 16, /*closed=*/false, m));
    MeshStats s = Stats(m);
    CHECK(s.watertight);
    CHECK(s.boundaryEdges == 0);
    CHECK(SignedVolume(m) > 0.0f);
}

void TestLatheOpenProfileHasBoundary()
{
    const std::vector<vec2> profile = {{0.5f, -0.5f}, {0.5f, 0.5f}};
    const uint32_t sectors = 8;
    MeshData m;
    CHECK(BuildLathe(profile, sectors, /*closed=*/false, m));
    MeshStats s = Stats(m);
    CHECK(!s.watertight);
    CHECK(s.boundaryEdges == 2 * sectors); // one open ring per end
}

void TestLatheCupWallProfile()
{
    // Up the outside, across the rim, down the inside: a wall with thickness.
    // closed=true caps the bottom (r=0.5) and the inner floor (r=0.4).
    const std::vector<vec2> profile = {
        {0.5f, 0.0f}, {0.5f, 1.0f}, {0.4f, 1.0f}, {0.4f, 0.1f}};
    MeshData m;
    CHECK(BuildLathe(profile, 24, true, m));
    MeshStats s = Stats(m);
    CHECK(s.watertight);
    CHECK(SignedVolume(m) > 0.0f);
    // Inner-wall vertex normals point toward the axis (descending profile leg).
    const uint32_t ringStride = 24 + 1;
    const Vertex& inner = m.vertices[3 * ringStride]; // ring 3 = (0.4, 0.1), theta = 0
    CHECK(inner.normal.x < -0.9f);
    // Outer-wall normals point away from it.
    const Vertex& outer = m.vertices[0]; // ring 0 = (0.5, 0), theta = 0
    CHECK(outer.normal.x > 0.5f);
}

void TestLatheProfileFidelity()
{
    // Every ring vertex sits at exactly its profile point's radius and height.
    const std::vector<vec2> profile = {
        {0.1f, 0.0f}, {0.45f, 0.3f}, {0.25f, 0.7f}, {0.35f, 1.0f}};
    const uint32_t sectors = 12;
    MeshData m;
    CHECK(BuildLathe(profile, sectors, false, m));
    CHECK(m.vertices.size() == profile.size() * (sectors + 1));
    for (size_t i = 0; i < profile.size(); ++i) {
        for (uint32_t s = 0; s <= sectors; ++s) {
            const vec3& p = m.vertices[i * (sectors + 1) + s].position;
            CHECK(ApproxEq(std::sqrt(p.x * p.x + p.z * p.z), profile[i].x, 1e-5f));
            CHECK(ApproxEq(p.y, profile[i].y, 0.0f));
        }
    }
}

void TestLatheSeamIsBitExact()
{
    // The duplicated UV seam column must reuse the exact s=0 floats, or the
    // position weld sees a phantom boundary (#117).
    const std::vector<vec2> profile = {{0.3f, 0.0f}, {0.5f, 0.4f}, {0.2f, 1.0f}};
    const uint32_t sectors = 48;
    MeshData m;
    CHECK(BuildLathe(profile, sectors, true, m));
    for (size_t i = 0; i < profile.size(); ++i) {
        const Vertex& first = m.vertices[i * (sectors + 1)];
        const Vertex& seam = m.vertices[i * (sectors + 1) + sectors];
        CHECK(SamePositionBits(first, seam));
        CHECK(ApproxEq(seam.uv.x, 1.0f, 0.0f));
    }
}

void TestLatheRejectsDegenerates()
{
    MeshData m;
    CHECK(!BuildLathe({}, 8, true, m));
    CHECK(!BuildLathe({{0.5f, 0.0f}}, 8, true, m)); // single point
    CHECK(!BuildLathe({{0.5f, 0.0f}, {0.5f, 0.0f}}, 8, true, m)); // duplicates only
    CHECK(!BuildLathe({{0.0f, 0.0f}, {0.0f, 1.0f}}, 8, true, m)); // all on the axis
    CHECK(!BuildLathe({{-0.5f, 0.0f}, {0.5f, 1.0f}}, 8, true, m)); // negative radius
    const float nan = std::nanf("");
    CHECK(!BuildLathe({{0.5f, nan}, {0.5f, 1.0f}}, 8, true, m));
    CHECK(m.vertices.empty() && m.indices.empty());
}

void TestLatheSwitchbackProfileStaysFinite()
{
    // A turnaround point (prev == next) collapses the central-difference
    // tangent; the one-sided fallback must keep every normal finite.
    const std::vector<vec2> profile = {{1.0f, 0.0f}, {2.0f, 1.0f}, {1.0f, 0.0f}};
    MeshData m;
    CHECK(BuildLathe(profile, 8, true, m));
    CHECK(AllFinite(m));
}

void TestLatheLoopProfileIsWatertightWithClosed()
{
    // A profile that returns to its start (revolved ring) closes itself; caps
    // must be skipped or they stack non-manifold disks on the seam ring.
    const std::vector<vec2> profile = {
        {1.0f, 0.0f}, {2.0f, 0.5f}, {1.0f, 1.0f}, {0.5f, 0.5f}, {1.0f, 0.0f}};
    MeshData m;
    CHECK(BuildLathe(profile, 8, /*closed=*/true, m));
    MeshStats s = Stats(m);
    CHECK(s.watertight);
    CHECK(s.nonManifoldEdges == 0);
    CHECK(SignedVolume(m) > 0.0f);
    CHECK(AllFinite(m));
}

void TestLatheRejectsOversizedInput()
{
    // Point lists come straight from scripts; runaway loops must fail fast
    // instead of allocating gigabytes on the GL main thread.
    std::vector<vec2> big;
    for (int i = 0; i < 5000; ++i)
        big.push_back({0.5f, (float)i * 0.001f});
    MeshData m;
    CHECK(!BuildLathe(big, 8, true, m)); // > 4096 profile points
    big.resize(2500);
    CHECK(!BuildLathe(big, 1024, true, m)); // 2500 * 1025 > 2M projected verts
    CHECK(BuildLathe(big, 48, true, m)); // same profile, sane grid: fine
}

void TestLatheSectorClamp()
{
    const std::vector<vec2> profile = {{0.5f, 0.0f}, {0.5f, 1.0f}};
    MeshData m;
    CHECK(BuildLathe(profile, 0, false, m));
    CHECK(m.vertices.size() == 2 * (3 + 1)); // clamped up to 3
    CHECK(BuildLathe(profile, 100000, false, m));
    CHECK(m.vertices.size() == 2 * (1024 + 1)); // clamped down to 1024
}

// --- sweep -----------------------------------------------------------------

std::vector<vec2> SquareSection(float half)
{
    // CCW: left across the top, down, right across the bottom, up.
    return {{half, half}, {-half, half}, {-half, -half}, {half, -half}};
}

void TestSweepStraightPathIsPrism()
{
    const std::vector<vec3> path = {{0, 0, 0}, {0, 2, 0}};
    MeshData m;
    CHECK(BuildSweep(SquareSection(0.1f), path, m));
    // 2 wall rings of k+1, plus 2 caps of center + k+1.
    CHECK(m.vertices.size() == 2 * 5 + 2 * 6);
    MeshStats s = Stats(m);
    CHECK(s.watertight);
    CHECK(s.boundaryEdges == 0);
    CHECK(s.degenerateTriangles == 0);
    CHECK(ApproxEq(SignedVolume(m), 0.2f * 0.2f * 2.0f, 1e-5f));
    // Side normals point away from the path axis.
    for (size_t j = 0; j < 5; ++j) {
        const Vertex& v = m.vertices[j];
        const vec3 out = v.position - vec3(0, v.position.y, 0);
        CHECK(glm::dot(v.normal, out) > 0.0f);
    }
}

void BoundsOf(const MeshData& m, vec3& lo, vec3& hi)
{
    lo = vec3(std::numeric_limits<float>::max());
    hi = -lo;
    for (const Vertex& v : m.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
    }
}

void TestSweepSectionOrientationHorizontalPath()
{
    // #138: pins the absolute frame convention — looking back along the start
    // tangent with world +Y as screen-up, the section reads as drawn, so a +Z
    // path maps section (x, y) onto world (x, y). A right triangle is
    // asymmetric under every 90-degree rotation and mirror, so any other
    // frame produces different bounds.
    const std::vector<vec2> tri = {{0, 0}, {2, 0}, {0, 1}};
    MeshData m;
    CHECK(BuildSweep(tri, {{0, 0, 0}, {0, 0, 1}}, m));
    CHECK(Stats(m).watertight);
    vec3 lo, hi;
    BoundsOf(m, lo, hi);
    CHECK(ApproxEq(lo.x, 0.0f, 1e-6f));
    CHECK(ApproxEq(hi.x, 2.0f, 1e-6f));
    CHECK(ApproxEq(lo.y, 0.0f, 1e-6f));
    CHECK(ApproxEq(hi.y, 1.0f, 1e-6f));
    CHECK(ApproxEq(lo.z, 0.0f, 1e-6f));
    CHECK(ApproxEq(hi.z, 1.0f, 1e-6f));
}

void TestSweepSectionOrientationVerticalPath()
{
    // Near-vertical fallback (-sign(tangent.y)*Z up hint): a +Y path maps
    // section (x, y) to world (x, -z), bit-compatible with the frame every
    // pre-#138 upward sweep already had.
    const std::vector<vec2> tri = {{0, 0}, {2, 0}, {0, 1}};
    MeshData m;
    CHECK(BuildSweep(tri, {{0, 0, 0}, {0, 2, 0}}, m));
    CHECK(Stats(m).watertight);
    vec3 lo, hi;
    BoundsOf(m, lo, hi);
    CHECK(ApproxEq(lo.x, 0.0f, 1e-6f));
    CHECK(ApproxEq(hi.x, 2.0f, 1e-6f));
    CHECK(ApproxEq(lo.y, 0.0f, 1e-6f));
    CHECK(ApproxEq(hi.y, 2.0f, 1e-6f));
    CHECK(ApproxEq(lo.z, -1.0f, 1e-6f));
    CHECK(ApproxEq(hi.z, 0.0f, 1e-6f));
}

void TestSweepSectionOrientationDownwardPath()
{
    // The hint flips sign with the tangent: a -Y path maps section (x, y) to
    // world (x, z) — also the exact pre-#138 frame. An unsigned -Z hint would
    // rotate legacy downward sweeps 180 degrees (reviewer catch on #139).
    const std::vector<vec2> tri = {{0, 0}, {2, 0}, {0, 1}};
    MeshData m;
    CHECK(BuildSweep(tri, {{0, 0, 0}, {0, -2, 0}}, m));
    CHECK(Stats(m).watertight);
    vec3 lo, hi;
    BoundsOf(m, lo, hi);
    CHECK(ApproxEq(lo.x, 0.0f, 1e-6f));
    CHECK(ApproxEq(hi.x, 2.0f, 1e-6f));
    CHECK(ApproxEq(lo.y, -2.0f, 1e-6f));
    CHECK(ApproxEq(hi.y, 0.0f, 1e-6f));
    CHECK(ApproxEq(lo.z, 0.0f, 1e-6f));
    CHECK(ApproxEq(hi.z, 1.0f, 1e-6f));
}

void TestSweepNormalizesSectionWinding()
{
    // Clockwise input must produce the identical outward-oriented solid.
    std::vector<vec2> cw = SquareSection(0.1f);
    std::reverse(cw.begin(), cw.end());
    const std::vector<vec3> path = {{0, 0, 0}, {0, 2, 0}};
    MeshData m;
    CHECK(BuildSweep(cw, path, m));
    CHECK(Stats(m).watertight);
    CHECK(SignedVolume(m) > 0.0f);
}

void TestSweepBentPathKeepsSectionRigid()
{
    // Parallel transport must carry the section through a 90-degree bend
    // without shearing: every ring vertex stays at its section distance from
    // the path point.
    const std::vector<vec3> path = {{0, 0, 0}, {0, 1, 0}, {1, 2, 0}, {2, 2, 0}};
    MeshData m;
    CHECK(BuildSweep(SquareSection(0.1f), path, m));
    const float want = glm::length(vec2(0.1f, 0.1f));
    const size_t ringStride = 5;
    for (size_t i = 0; i < path.size(); ++i)
        for (size_t j = 0; j < ringStride; ++j) {
            const vec3& p = m.vertices[i * ringStride + j].position;
            CHECK(ApproxEq(glm::length(p - path[i]), want, 1e-5f));
        }
    MeshStats s = Stats(m);
    CHECK(s.watertight);
    CHECK(SignedVolume(m) > 0.0f);
}

void TestSweepSeamIsBitExact()
{
    const std::vector<vec3> path = {{0, 0, 0}, {0.5f, 1.2f, 0.3f}, {1, 2, 1}};
    MeshData m;
    CHECK(BuildSweep(SquareSection(0.2f), path, m));
    const size_t ringStride = 5;
    for (size_t i = 0; i < path.size(); ++i) {
        const Vertex& first = m.vertices[i * ringStride];
        const Vertex& seam = m.vertices[i * ringStride + 4];
        CHECK(SamePositionBits(first, seam));
    }
}

void TestSweepClosedInputPolygonTolerated()
{
    // A profile whose last point repeats the first must not create a
    // zero-length wall column.
    std::vector<vec2> sec = SquareSection(0.1f);
    sec.push_back(sec.front());
    const std::vector<vec3> path = {{0, 0, 0}, {0, 1, 0}};
    MeshData m;
    CHECK(BuildSweep(sec, path, m));
    CHECK(Stats(m).watertight);
    CHECK(ApproxEq(SignedVolume(m), 0.2f * 0.2f * 1.0f, 1e-5f));
}

void TestSweepSwitchbackPathStaysFinite()
{
    // Out-and-back path: interior tangents see prev == next. Without the
    // one-sided fallback the NaN frame poisons every ring POSITION.
    const std::vector<vec3> path = {{0, 0, 0}, {0, 1, 0}, {0, 0, 0}};
    MeshData m;
    CHECK(BuildSweep(SquareSection(0.1f), path, m));
    CHECK(AllFinite(m));
}

void TestSweepRejectsOversizedInput()
{
    std::vector<vec3> longPath;
    for (int i = 0; i < 20000; ++i)
        longPath.push_back({0.0f, (float)i * 0.01f, 0.0f});
    MeshData m;
    CHECK(!BuildSweep(SquareSection(0.1f), longPath, m)); // > 16384 path points
    longPath.resize(16000);
    std::vector<vec2> bigSec;
    for (int i = 0; i < 200; ++i) {
        const float a = 2.0f * 3.14159265f * (float)i / 200.0f;
        bigSec.push_back({0.1f * std::cos(a), 0.1f * std::sin(a)});
    }
    CHECK(!BuildSweep(bigSec, longPath, m)); // 16000 * 201 > 2M projected verts
}

void TestSweepRejectsDegenerates()
{
    MeshData m;
    const std::vector<vec3> path = {{0, 0, 0}, {0, 1, 0}};
    CHECK(!BuildSweep({}, path, m));
    CHECK(!BuildSweep({{0, 0}, {1, 0}}, path, m)); // two points: no area
    CHECK(!BuildSweep({{0, 0}, {1, 0}, {2, 0}}, path, m)); // collinear: no area
    CHECK(!BuildSweep(SquareSection(0.1f), {{0, 0, 0}}, m)); // single path point
    CHECK(!BuildSweep(SquareSection(0.1f), {{0, 0, 0}, {0, 0, 0}}, m)); // zero length
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(!BuildSweep(SquareSection(0.1f), {{0, 0, 0}, {0, inf, 0}}, m));
    CHECK(m.vertices.empty() && m.indices.empty());
}

} // namespace

void RunMeshBuildTests()
{
    TestLatheCylinderProfile();
    TestLathePoleClosedProfileIsWatertight();
    TestLatheOpenProfileHasBoundary();
    TestLatheCupWallProfile();
    TestLatheProfileFidelity();
    TestLatheSeamIsBitExact();
    TestLatheRejectsDegenerates();
    TestLatheSwitchbackProfileStaysFinite();
    TestLatheLoopProfileIsWatertightWithClosed();
    TestLatheRejectsOversizedInput();
    TestLatheSectorClamp();
    TestSweepStraightPathIsPrism();
    TestSweepSectionOrientationHorizontalPath();
    TestSweepSectionOrientationVerticalPath();
    TestSweepSectionOrientationDownwardPath();
    TestSweepNormalizesSectionWinding();
    TestSweepBentPathKeepsSectionRigid();
    TestSweepSeamIsBitExact();
    TestSweepClosedInputPolygonTolerated();
    TestSweepSwitchbackPathStaysFinite();
    TestSweepRejectsOversizedInput();
    TestSweepRejectsDegenerates();
}

} // namespace forge::test
