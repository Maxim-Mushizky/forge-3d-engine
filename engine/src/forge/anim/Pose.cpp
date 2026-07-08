#include "Pose.h"

#include "forge/anim/Skeleton.h"

#include <glm/gtc/quaternion.hpp>

namespace forge {

std::vector<quat> PoseLocalRotations(const Skeleton& skeleton, const Pose& pose)
{
    std::vector<quat> r = skeleton.bindR; // copy; bind is the baseline
    // A mis-sized pose (or empty) is treated as "no override" rather than asserting:
    // a Pose can outlive a re-import that changed the joint count, and posing must
    // degrade to bind rather than crash. Same defensive stance as SkinVertices.
    if (pose.deltas.size() != skeleton.JointCount())
        return r;
    for (size_t i = 0; i < r.size(); ++i)
        r[i] = glm::normalize(skeleton.bindR[i] * pose.deltas[i]);
    return r;
}

} // namespace forge
