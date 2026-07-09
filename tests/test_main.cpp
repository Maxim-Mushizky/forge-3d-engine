#include "test_framework.h"

#include <cstdio>

namespace forge::test {
void RunGeometryTests();
void RunUuidTests();
void RunSceneFormatTests();
void RunBoxSelectTests();
void RunDropToGroundTests();
void RunBvhTests();
void RunSnapTests();
void RunEditMeshTests();
void RunDropRouterTests();
void RunSettingsTests();
void RunMcpTests();
void RunMcpScriptTests();
void RunCommandStackTests();
void RunMcpViewsTests();
void RunMeshStatsTests();
void RunUvUnwrapTests();
void RunTextureGenTests();
void RunMeshBuildTests();
void RunSpatialTests();
void RunPlacementTests();
void RunMcpElementsTests();
void RunMcpSculptTests();
void RunPolyHavenTests();
void RunSilhouetteTests();
void RunSssTests();
void RunSkeletonTests();
void RunPoseTests();
void RunIkTests();
void RunMorphTests();
} // namespace forge::test

int main()
{
    using namespace forge::test;

    RunGeometryTests();
    RunUuidTests();
    RunSceneFormatTests();
    RunBoxSelectTests();
    RunDropToGroundTests();
    RunBvhTests();
    RunSnapTests();
    RunEditMeshTests();
    RunDropRouterTests();
    RunSettingsTests();
    RunMcpTests();
    RunMcpScriptTests();
    RunCommandStackTests();
    RunMcpViewsTests();
    RunMeshStatsTests();
    RunUvUnwrapTests();
    RunTextureGenTests();
    RunMeshBuildTests();
    RunSpatialTests();
    RunPlacementTests();
    RunMcpElementsTests();
    RunMcpSculptTests();
    RunPolyHavenTests();
    RunSilhouetteTests();
    RunSssTests();
    RunSkeletonTests();
    RunPoseTests();
    RunIkTests();
    RunMorphTests();

    if (g_failures == 0) {
        std::printf("[ok] all tests passed\n");
        return 0;
    }
    std::printf("[FAIL] %d check(s) failed\n", g_failures);
    return 1;
}
