#include "test_framework.h"

#include "mcp/McpViews.h"

#include <glm/gtc/matrix_transform.hpp>

#include <set>

// Suites for the render_views camera kernel (#93): presets, projection,
// id colors, and world-AABB transforms — all GL-free.

namespace forge::test {

static AABB UnitBoxAt(const vec3& center, const vec3& halfExtents)
{
    AABB b;
    b.Expand(center - halfExtents);
    b.Expand(center + halfExtents);
    return b;
}

static void PresetShapes()
{
    const AABB box = UnitBoxAt({1.0f, 2.0f, 3.0f}, {1.0f, 1.0f, 1.0f});
    const vec3 center{1.0f, 2.0f, 3.0f};

    auto specs = BuildViewSpecs("turntable", box);
    CHECK(specs.size() == 4);
    CHECK(specs[0].label == "front");
    CHECK(specs[2].label == "back");
    // All orbit eyes sit at the same distance from the target center.
    const float d0 = glm::length(specs[0].eye - center);
    for (const ViewSpec& s : specs) {
        CHECK(ApproxEq(glm::length(s.eye - center), d0, 1e-3f));
        CHECK(!s.ortho);
        CHECK(glm::length(s.center - center) < 1e-4f);
    }
    // Front eye is on the +z side, back on -z.
    CHECK(specs[0].eye.z > center.z);
    CHECK(specs[2].eye.z < center.z);

    specs = BuildViewSpecs("4up", box);
    CHECK(specs.size() == 4);
    CHECK(specs[2].ortho); // top view
    CHECK(specs[2].eye.y > center.y);
    CHECK(ApproxEq(specs[2].up.z, -1.0f));

    specs = BuildViewSpecs("top_ortho", box);
    CHECK(specs.size() == 1);
    CHECK(specs[0].ortho);
    CHECK(specs[0].orthoHalf > 0.0f);

    CHECK(BuildViewSpecs("sideways", box).empty()); // unknown preset
    CHECK(BuildViewSpecs("4up", AABB{}).empty());   // invalid target
}

static void ProjectionCentersTarget()
{
    const AABB box = UnitBoxAt({-2.0f, 0.5f, 4.0f}, {0.5f, 0.5f, 0.5f});
    for (const char* preset : {"turntable", "4up", "top_ortho"}) {
        for (const ViewSpec& s : BuildViewSpecs(preset, box)) {
            const mat4 vp = ViewProjFor(s, 1.0f);
            vec4 clip = vp * vec4(s.center, 1.0f);
            CHECK(clip.w > 0.0f); // target in front of the camera
            const vec2 ndc = vec2(clip) / clip.w;
            CHECK(ApproxEq(ndc.x, 0.0f, 1e-3f)); // framed dead-center
            CHECK(ApproxEq(ndc.y, 0.0f, 1e-3f));
            // The box corners stay inside the frustum with margin.
            vec4 corner = vp * vec4(box.max, 1.0f);
            CHECK(std::abs(corner.x / corner.w) < 1.0f);
            CHECK(std::abs(corner.y / corner.w) < 1.0f);
        }
    }
}

static void IdColorsDistinct()
{
    std::set<std::tuple<int, int, int>> seen;
    for (size_t i = 0; i < 16; ++i) {
        const vec3 c = IdColor(i);
        CHECK(c.r >= 0.0f && c.r <= 1.0f);
        CHECK(c.g >= 0.0f && c.g <= 1.0f);
        CHECK(c.b >= 0.0f && c.b <= 1.0f);
        seen.insert({(int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255)});
    }
    CHECK(seen.size() == 16); // no collisions across the first 16 entities
}

static void TransformAABBRotates()
{
    // A 2x1x1 box rotated 90 deg about Y swaps its x/z extents.
    const AABB box = UnitBoxAt({0.0f, 0.0f, 0.0f}, {1.0f, 0.5f, 0.5f});
    const mat4 rot = glm::rotate(mat4(1.0f), glm::radians(90.0f), vec3(0, 1, 0));
    const AABB r = TransformAABB(box, rot);
    CHECK(ApproxEq(r.max.x - r.min.x, 1.0f, 1e-4f));
    CHECK(ApproxEq(r.max.z - r.min.z, 2.0f, 1e-4f));
    CHECK(ApproxEq(r.max.y - r.min.y, 1.0f, 1e-4f));

    // Translation just shifts.
    const mat4 t = glm::translate(mat4(1.0f), vec3(5.0f, 0.0f, 0.0f));
    const AABB moved = TransformAABB(box, t);
    CHECK(ApproxEq(moved.min.x, 4.0f, 1e-4f));
    CHECK(ApproxEq(moved.max.x, 6.0f, 1e-4f));
}

void RunMcpViewsTests()
{
    PresetShapes();
    ProjectionCentersTarget();
    IdColorsDistinct();
    TransformAABBRotates();
    std::printf("[ok] mcp views kernel tests\n");
}

} // namespace forge::test
