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

    if (g_failures == 0) {
        std::printf("[ok] all tests passed\n");
        return 0;
    }
    std::printf("[FAIL] %d check(s) failed\n", g_failures);
    return 1;
}
