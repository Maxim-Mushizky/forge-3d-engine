#include "Skeleton.h"

#include "forge/core/Log.h"

#include <algorithm>

namespace forge {

std::vector<mat4> ComputeGlobalTransforms(const Skeleton& skeleton, const std::vector<vec3>& t,
                                          const std::vector<quat>& r, const std::vector<vec3>& s)
{
    const size_t count = skeleton.JointCount();
    std::vector<mat4> globals(count, mat4(1.0f));
    if (t.size() != count || r.size() != count || s.size() != count) {
        // Pose data ultimately comes from files/scripts: degrade, never assert.
        FORGE_WARN("Skeleton: pose has %zu/%zu/%zu TRS entries for %zu joints — identity globals",
                   t.size(), r.size(), s.size(), count);
        return globals;
    }
    for (size_t i = 0; i < count; ++i) {
        mat4 local = glm::translate(mat4(1.0f), t[i]) * glm::mat4_cast(r[i]) *
                     glm::scale(mat4(1.0f), s[i]);
        // parents[i] < i is the struct invariant; the range check just keeps a
        // hand-built skeleton with a bad parent from reading out of bounds.
        int parent = skeleton.parents[i];
        globals[i] = (parent >= 0 && parent < (int)i) ? globals[parent] * local : local;
    }
    return globals;
}

std::vector<mat4> ComputeBindGlobals(const Skeleton& skeleton)
{
    return ComputeGlobalTransforms(skeleton, skeleton.bindT, skeleton.bindR, skeleton.bindS);
}

std::vector<mat4> ComputePalette(const std::vector<mat4>& globals,
                                 const std::vector<mat4>& inverseBind)
{
    if (globals.size() != inverseBind.size())
        FORGE_WARN("Skeleton: %zu globals vs %zu inverse-bind matrices — palette truncated",
                   globals.size(), inverseBind.size());
    const size_t count = std::min(globals.size(), inverseBind.size());
    std::vector<mat4> palette(count);
    for (size_t i = 0; i < count; ++i)
        palette[i] = globals[i] * inverseBind[i];
    return palette;
}

} // namespace forge
