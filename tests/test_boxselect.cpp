#include "test_framework.h"

#include <forge/scene/BoxSelect.h>

#include <glm/gtc/matrix_transform.hpp>

namespace forge::test {

void RunBoxSelectTests()
{
    // Camera at +10z looking at the origin, standard perspective.
    mat4 view = glm::lookAt(vec3(0.0f, 0.0f, 10.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
    mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    mat4 vp = proj * view;

    // --- box at the origin projects to a centered rect -----------------------
    {
        AABB box{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
        auto rect = ProjectAABBToScreen(vp, box);
        CHECK(rect.has_value());
        if (rect) {
            CHECK(rect->min.x > 0.3f && rect->max.x < 0.7f); // roughly centered
            CHECK(rect->min.y > 0.2f && rect->max.y < 0.8f);
            CHECK(rect->min.x < 0.5f && rect->max.x > 0.5f); // spans the center
            CHECK(rect->min.y < 0.5f && rect->max.y > 0.5f);
        }
    }

    // --- box fully behind the camera -> nullopt -------------------------------
    {
        AABB box{{-1.0f, -1.0f, 19.0f}, {1.0f, 1.0f, 21.0f}}; // z=20, camera at z=10 facing -z
        CHECK(!ProjectAABBToScreen(vp, box).has_value());
    }

    // --- box straddling the near plane -> conservative full rect --------------
    {
        AABB box{{-1.0f, -1.0f, 5.0f}, {1.0f, 1.0f, 15.0f}}; // spans the camera position
        auto rect = ProjectAABBToScreen(vp, box);
        CHECK(rect.has_value());
        if (rect) {
            CHECK(ApproxEq(rect->min.x, 0.0f) && ApproxEq(rect->min.y, 0.0f));
            CHECK(ApproxEq(rect->max.x, 1.0f) && ApproxEq(rect->max.y, 1.0f));
        }
    }

    // --- invalid (default, empty) AABB -> nullopt ------------------------------
    {
        CHECK(!ProjectAABBToScreen(vp, AABB{}).has_value());
    }

    // --- off-center box lands on the correct side -----------------------------
    {
        AABB left{{-6.0f, -0.5f, -0.5f}, {-5.0f, 0.5f, 0.5f}};
        auto rect = ProjectAABBToScreen(vp, left);
        CHECK(rect.has_value());
        if (rect)
            CHECK(rect->max.x < 0.5f); // entirely on the left half of the screen
    }

    // --- rect overlap basics ----------------------------------------------------
    {
        RectUV a{{0.1f, 0.1f}, {0.4f, 0.4f}};
        RectUV b{{0.3f, 0.3f}, {0.6f, 0.6f}}; // overlaps a
        RectUV c{{0.5f, 0.5f}, {0.9f, 0.9f}}; // touches b at a corner, misses a
        CHECK(RectsOverlap(a, b));
        CHECK(RectsOverlap(b, a));
        CHECK(!RectsOverlap(a, c));
        CHECK(RectsOverlap(b, c)); // shared edge point counts as overlap
        // containment counts as overlap
        RectUV inner{{0.2f, 0.2f}, {0.3f, 0.3f}};
        CHECK(RectsOverlap(a, inner));
    }
}

} // namespace forge::test
