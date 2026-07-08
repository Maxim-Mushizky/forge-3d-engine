#include "PosePresets.h"

#include "forge/anim/Skeleton.h"

#include <glm/gtc/quaternion.hpp>

#include <utility>
#include <vector>

namespace forge {

namespace {

// One preset row: rotate the named joint by these euler degrees (delta from
// bind). Angles target the standard Mixamo humanoid names; signs were chosen
// so the bends look natural on a Y-up, Z-forward rig — tune against the
// acceptance asset, the exact values are not load-bearing.
using PresetRow = std::pair<const char*, vec3>;

// a-pose: bring the arms down from a T.
const std::vector<PresetRow> kAPose = {
    {"LeftArm", {0, 0, -45}},
    {"RightArm", {0, 0, 45}},
};
// t-pose: identity — T is the bind pose for most humanoid rigs.
const std::vector<PresetRow> kTPose = {
    {"LeftArm", {0, 0, 0}},
    {"RightArm", {0, 0, 0}},
};
// sit: fold hips and knees ~90 degrees.
const std::vector<PresetRow> kSit = {
    {"LeftUpLeg", {-90, 0, 0}},
    {"RightUpLeg", {-90, 0, 0}},
    {"LeftLeg", {90, 0, 0}},
    {"RightLeg", {90, 0, 0}},
};

// Full-size identity pose, then overwrite the rows whose joints exist. A full
// (non-empty) result is deliberate even for all-identity: it CLEARS any prior
// per-joint override instead of leaving it in place.
Pose PoseFromRows(const Skeleton& skeleton, const std::vector<PresetRow>& rows)
{
    Pose pose;
    pose.deltas.assign(skeleton.JointCount(), quat(1, 0, 0, 0));
    for (const PresetRow& row : rows) {
        const int j = JointIndex(skeleton, row.first);
        if (j >= 0)
            pose.deltas[(size_t)j] = quat(glm::radians(row.second));
    }
    return pose;
}

} // namespace

std::optional<Pose> MakePresetPose(const Skeleton& skeleton, const std::string& preset)
{
    if (preset == "rest")
        return PoseFromRows(skeleton, {}); // all identity: back to bind
    if (preset == "t-pose")
        return PoseFromRows(skeleton, kTPose);
    if (preset == "a-pose")
        return PoseFromRows(skeleton, kAPose);
    if (preset == "sit")
        return PoseFromRows(skeleton, kSit);
    return std::nullopt;
}

} // namespace forge
