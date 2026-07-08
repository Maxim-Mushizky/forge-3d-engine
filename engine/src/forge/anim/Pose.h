#pragma once

#include "forge/core/Math.h" // glm quat/vec3

#include <vector>

namespace forge {

struct Skeleton; // fwd

// Runtime FK pose: per-joint LOCAL-rotation delta applied on top of the skeleton's
// bind rotation. Effective local rotation of joint i = bindR[i] * deltas[i], i.e. the
// delta rotates the bone in its own rest-oriented frame ("bend this joint"). An
// identity delta = rest/bind. Empty deltas = pure bind pose (no override at all).
// Deliberately tiny + copyable: the undo system stores the whole Pose (a few hundred
// bytes for a humanoid), never a mesh snapshot (#147). Lives by value on Entity.
struct Pose {
    std::vector<quat> deltas; // empty = bind; else size == skeleton JointCount

    bool Empty() const { return deltas.empty(); }
};

// Effective per-joint LOCAL rotations for this pose against a skeleton:
//   result[i] = normalize(bindR[i] * deltas[i])   when deltas is sized to JointCount
//   result    = skeleton.bindR                    when the pose is empty OR mis-sized
// GL-free and pure so the skinning path stays unit-testable. This is the ONLY place
// bind and pose compose; ApplyPose (GL-adjacent) calls it, then reuses the existing
// ComputeGlobalTransforms(skeleton, bindT, r, bindS) seam verbatim.
std::vector<quat> PoseLocalRotations(const Skeleton& skeleton, const Pose& pose);

} // namespace forge
