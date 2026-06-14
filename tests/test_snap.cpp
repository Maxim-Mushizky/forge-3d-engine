#include "test_framework.h"

#include <forge/scene/TransformSnap.h>

namespace forge::test {

void RunSnapTests()
{
    // --- scalar: rounds to the nearest grid multiple --------------------------
    {
        CHECK(ApproxEq(SnapToStep(0.03f, 0.25f), 0.0f));  // 0.12 of a step -> down
        CHECK(ApproxEq(SnapToStep(0.13f, 0.25f), 0.25f)); // 0.52 of a step -> up
        CHECK(ApproxEq(SnapToStep(0.70f, 0.25f), 0.75f));
        CHECK(ApproxEq(SnapToStep(0.50f, 0.25f), 0.50f)); // exact multiple is unchanged
    }

    // --- scalar: negatives round symmetrically --------------------------------
    {
        CHECK(ApproxEq(SnapToStep(-0.13f, 0.25f), -0.25f));
        CHECK(ApproxEq(SnapToStep(-0.10f, 0.25f), 0.0f)); // 0.4 of a step -> toward zero
    }

    // --- scalar: a non-positive step is "snapping off" — value passes through -
    {
        CHECK(ApproxEq(SnapToStep(0.37f, 0.0f), 0.37f));
        CHECK(ApproxEq(SnapToStep(0.37f, -1.0f), 0.37f));
    }

    // --- rotation: degrees quantize to the angle step -------------------------
    {
        CHECK(ApproxEq(SnapToStep(7.0f, 15.0f), 0.0f));   // < half a step
        CHECK(ApproxEq(SnapToStep(8.0f, 15.0f), 15.0f));  // > half a step
        CHECK(ApproxEq(SnapToStep(40.0f, 45.0f), 45.0f));
        CHECK(ApproxEq(SnapToStep(12.0f, 5.0f), 10.0f));
    }

    // --- scale: factor step ----------------------------------------------------
    {
        CHECK(ApproxEq(SnapToStep(1.07f, 0.1f), 1.1f));
        CHECK(ApproxEq(SnapToStep(1.04f, 0.1f), 1.0f));
    }

    // --- vec3 overload snaps each component independently ---------------------
    {
        vec3 r = SnapToStep(vec3{0.03f, 0.70f, -0.13f}, 0.25f);
        CHECK(ApproxEq(r.x, 0.0f));
        CHECK(ApproxEq(r.y, 0.75f));
        CHECK(ApproxEq(r.z, -0.25f));
    }

    // --- vec3 with snapping off is a no-op ------------------------------------
    {
        vec3 in{1.234f, -5.678f, 0.001f};
        vec3 r = SnapToStep(in, 0.0f);
        CHECK(ApproxEq(r.x, in.x));
        CHECK(ApproxEq(r.y, in.y));
        CHECK(ApproxEq(r.z, in.z));
    }
}

} // namespace forge::test
