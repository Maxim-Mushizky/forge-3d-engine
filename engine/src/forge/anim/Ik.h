#pragma once

#include "forge/core/Math.h" // glm quat/vec3

#include <optional>

namespace forge {

struct Skeleton; // fwd
struct Pose;     // fwd

// Analytic pose solvers on top of the #147 Pose model: two-bone IK and single-bone
// aim. GL-free and pure so they stay unit-testable — all the trig lives here, the
// GL-adjacent ApplyPose seam is untouched. Both write per-joint LOCAL rotation
// deltas into the Pose (the same "delta on top of bind" the FK path reads) and
// return false on inputs they cannot solve, leaving the Pose untouched.

// A root->mid->end joint triple for two-bone IK. root must be an ancestor of mid
// and mid an ancestor of end (validated by walking parents); they need not be
// immediate parents — intermediate joints ride along rigidly.
struct IkChain {
    int root = -1; // shoulder/hip
    int mid = -1;  // elbow/knee
    int end = -1;  // wrist/ankle — the joint driven onto the target
};

// Solve root+mid deltas so the end joint reaches targetModel (model/skin space).
// poleModel picks the bend side (which way the elbow points). Out-of-reach targets
// clamp to just under full extension; over-folded ones to just over full fold — the
// solve never NaNs (non-finite or overflow-huge inputs are rejected, not solved).
// Writes ONLY deltas[root] and deltas[mid]; the end joint's delta is left alone
// (the wrist keeps whatever pose it had). Returns false (Pose untouched) on: index
// out of range, a non-ancestry chain, a zero-length bone, or a non-finite/overflow
// target or pole. Exact for uniform bind scale; non-uniform bindS chains shear and
// the solve lands near, not on, the target (import already approximates shear away).
bool SolveTwoBoneIk(const Skeleton& skeleton, Pose& pose, const IkChain& chain,
                    const vec3& targetModel, const vec3& poleModel);

// Rotate `joint` so its forward axis points at targetModel (model/skin space).
// Forward is the bind-pose direction from `joint` toward `forwardChild` — rigs have
// no canonical axis, the child bone direction IS "forward". Optional upModel fixes
// the twist (look-rotation basis); when absent, or when up is collinear with the aim
// direction, the shortest-arc rotation is used and twist is left as-is. Writes only
// deltas[joint]. Returns false (Pose untouched) on: index out of range, forwardChild
// not a strict descendant of joint, a zero-length forward bone, a target on top of
// joint, or a non-finite/overflow target or up.
bool SolveAim(const Skeleton& skeleton, Pose& pose, int joint, int forwardChild,
              const vec3& targetModel, const std::optional<vec3>& upModel);

} // namespace forge
