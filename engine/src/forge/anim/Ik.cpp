#include "Ik.h"

#include "forge/anim/Pose.h"
#include "forge/anim/Skeleton.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace forge {

namespace {

// Below this, a vector is treated as having no reliable direction (bones, cross
// products, aim rays). Absolute unit-space epsilon: the rigs we pose are authored
// around 1.0 bone lengths, so a fixed threshold reads cleaner than a relative one.
constexpr float kEps = 1e-4f;

// The rotation carried by a joint global. Columns are R*scale_i (no shear from
// TRS locals), so normalizing each column recovers a pure rotation even when
// bindS != 1. quat_cast wants an orthonormal basis or it skews the result.
quat RotationOf(const mat4& m)
{
    mat3 r(m);
    for (int c = 0; c < 3; ++c) {
        float len = glm::length(r[c]);
        r[c] = (len > kEps) ? r[c] / len : vec3(c == 0, c == 1, c == 2);
    }
    return glm::normalize(glm::quat_cast(r));
}

// A unit axis orthogonal to u, built from the world axis u is *least* aligned with
// so the cross never collapses. Deterministic — same u always yields the same axis,
// which is what makes the antiparallel / degenerate fallbacks reproducible.
vec3 OrthogonalAxis(const vec3& u)
{
    const vec3 a = glm::abs(u);
    const vec3 world = (a.x <= a.y && a.x <= a.z) ? vec3(1, 0, 0)
                       : (a.y <= a.z)             ? vec3(0, 1, 0)
                                                  : vec3(0, 0, 1);
    vec3 axis = glm::cross(u, world);
    float len = glm::length(axis);
    if (len < kEps) // u ~ parallel to the picked axis (shouldn't happen: least-aligned)
        return vec3(1, 0, 0);
    return axis / len;
}

// Shortest-arc rotation taking unit u onto unit v. The antiparallel case (v == -u)
// has no unique axis, so we spin 180 deg about a deterministic orthogonal axis.
quat ShortestArc(const vec3& u, const vec3& v)
{
    const float d = glm::dot(u, v);
    if (d > 1.0f - kEps) // already aligned
        return quat(1, 0, 0, 0);
    if (d < -1.0f + kEps) // antiparallel: 180 deg about any axis orthogonal to u
        return glm::angleAxis(glm::pi<float>(), OrthogonalAxis(u));
    const vec3 c = glm::cross(u, v);
    return glm::normalize(quat(1.0f + d, c.x, c.y, c.z));
}

// Convert a desired MODEL-space rotation for `joint` into the local delta the Pose
// stores: effective local = bindR * delta, and local = inverse(parentGlobalRot) *
// desiredGlobal, so delta = inverse(bindR) * inverse(parentGlobalRot) * desiredGlobal.
// parentGlobalRot comes from whichever globals are current when this is called.
quat LocalDelta(const Skeleton& sk, int joint, const std::vector<mat4>& globals,
                const quat& desiredGlobalRot)
{
    const int parent = sk.parents[(size_t)joint];
    const quat parentRot =
        (parent >= 0 && parent < (int)globals.size()) ? RotationOf(globals[(size_t)parent])
                                                       : quat(1, 0, 0, 0);
    const quat desiredLocal = glm::inverse(parentRot) * desiredGlobalRot;
    return glm::normalize(glm::inverse(sk.bindR[(size_t)joint]) * desiredLocal);
}

bool InRange(const Skeleton& sk, int j) { return j >= 0 && j < (int)sk.JointCount(); }

// True when `anc` is a strict ancestor of `node` (walks parents; the SoA invariant
// parents[i] < i guarantees termination).
bool IsAncestor(const Skeleton& sk, int anc, int node)
{
    for (int p = sk.parents[(size_t)node]; p >= 0; p = sk.parents[(size_t)p])
        if (p == anc)
            return true;
    return false;
}

// Materialize an empty/mis-sized Pose to identity deltas so we can write individual
// joints — same lazy-widen the MCP set_pose handler does before mutating.
void EnsureSized(const Skeleton& sk, Pose& pose)
{
    if (pose.deltas.size() != sk.JointCount())
        pose.deltas.assign(sk.JointCount(), quat(1, 0, 0, 0));
}

vec3 Translation(const mat4& m) { return vec3(m[3]); }

} // namespace

bool SolveTwoBoneIk(const Skeleton& skeleton, Pose& pose, const IkChain& chain,
                    const vec3& targetModel, const vec3& poleModel)
{
    const Skeleton& sk = skeleton;
    if (!InRange(sk, chain.root) || !InRange(sk, chain.mid) || !InRange(sk, chain.end))
        return false;
    if (chain.root == chain.mid || chain.mid == chain.end || chain.root == chain.end)
        return false;
    if (!IsAncestor(sk, chain.root, chain.mid) || !IsAncestor(sk, chain.mid, chain.end))
        return false;

    Pose work = pose; // commit only on success — Pose stays untouched on any early out
    EnsureSized(sk, work);

    // --- current geometry (model space), bone lengths include bind scale ---------
    std::vector<quat> r = PoseLocalRotations(sk, work);
    std::vector<mat4> g = ComputeGlobalTransforms(sk, sk.bindT, r, sk.bindS);
    const vec3 A = Translation(g[(size_t)chain.root]);
    const vec3 B = Translation(g[(size_t)chain.mid]);
    const vec3 C = Translation(g[(size_t)chain.end]);
    const float L1 = glm::length(B - A);
    const float L2 = glm::length(C - B);
    if (L1 < kEps || L2 < kEps) // zero-length bone: no rotation can place the end
        return false;

    // --- reach clamp: both the extension and the fold ends are triangle singularities.
    const vec3 toT = targetModel - A;
    const float distToT = glm::length(toT);
    vec3 dirAT = (distToT > kEps) ? toT / distToT
                                  : glm::normalize(B - A); // target on the root: keep current aim
    float lo = std::fabs(L1 - L2) + kEps;
    float hi = L1 + L2 - kEps;
    if (hi < lo) // both bones near-degenerate; keep the solve finite
        hi = lo;
    const float d = glm::clamp(distToT, lo, hi);
    const vec3 tEff = A + d * dirAT; // the actually-reachable point the end lands on

    // --- bend plane: the pole picks the side; degeneracies fall back deterministically.
    vec3 n = glm::cross(dirAT, poleModel - A);
    if (glm::length(n) < kEps)
        n = glm::cross(C - A, B - A); // pole collinear with A->T: use the current-pose plane
    if (glm::length(n) < kEps)
        n = OrthogonalAxis(dirAT); // straight current chain too: any orthogonal axis
    n = glm::normalize(n);

    // --- law of cosines for the interior angle at the root; clamp acos domain -----
    const float cosA = glm::clamp((L1 * L1 + d * d - L2 * L2) / (2.0f * L1 * d), -1.0f, 1.0f);
    const float thetaA = std::acos(cosA);
    const vec3 bendDir = glm::angleAxis(thetaA, n) * dirAT; // +theta: n already encodes the side
    const vec3 Bnew = A + L1 * bendDir;

    // --- root: rotate (B-A) onto (Bnew-A), localize, write delta -----------------
    const quat dRootGlobal =
        ShortestArc(glm::normalize(B - A), glm::normalize(Bnew - A)) * RotationOf(g[(size_t)chain.root]);
    work.deltas[(size_t)chain.root] = LocalDelta(sk, chain.root, g, dRootGlobal);

    // --- RE-RUN FK: mid's parent frame moved when we rotated the root. Solving mid
    // against stale globals is the classic two-bone-IK bug; the refresh makes it
    // structurally impossible. ---------------------------------------------------
    r = PoseLocalRotations(sk, work);
    g = ComputeGlobalTransforms(sk, sk.bindT, r, sk.bindS);
    const vec3 Bref = Translation(g[(size_t)chain.mid]);
    const vec3 Cref = Translation(g[(size_t)chain.end]);
    const quat dMidGlobal =
        ShortestArc(glm::normalize(Cref - Bref), glm::normalize(tEff - Bref)) *
        RotationOf(g[(size_t)chain.mid]);
    work.deltas[(size_t)chain.mid] = LocalDelta(sk, chain.mid, g, dMidGlobal);

    pose = std::move(work);
    return true;
}

bool SolveAim(const Skeleton& skeleton, Pose& pose, int joint, int forwardChild,
              const vec3& targetModel, const std::optional<vec3>& upModel)
{
    const Skeleton& sk = skeleton;
    if (!InRange(sk, joint) || !InRange(sk, forwardChild) || joint == forwardChild)
        return false;

    Pose work = pose;
    EnsureSized(sk, work);

    // Forward axis in the joint's LOCAL frame, measured once from the BIND pose: the
    // bone toward forwardChild, expressed in the joint's own rest frame. Stays fixed
    // no matter how the joint is currently posed.
    const std::vector<mat4> bind = ComputeBindGlobals(sk);
    const vec3 bindBone = Translation(bind[(size_t)forwardChild]) - Translation(bind[(size_t)joint]);
    if (glm::length(bindBone) < kEps) // co-located joints: no bone direction to aim
        return false;
    const vec3 fLocal =
        glm::normalize(glm::inverse(RotationOf(bind[(size_t)joint])) * glm::normalize(bindBone));

    // Current joint frame and the aim ray.
    const std::vector<quat> r = PoseLocalRotations(sk, work);
    const std::vector<mat4> g = ComputeGlobalTransforms(sk, sk.bindT, r, sk.bindS);
    const quat curGlobal = RotationOf(g[(size_t)joint]);
    const vec3 jointPos = Translation(g[(size_t)joint]);
    const vec3 toTarget = targetModel - jointPos;
    if (glm::length(toTarget) < kEps) // target on top of the joint: no aim direction
        return false;
    const vec3 desired = glm::normalize(toTarget);

    quat desiredGlobal;
    bool useUp = false;
    if (upModel) {
        // Twist-stabilized: map (fLocal, a reference up) onto (desired, up-projected)
        // via an orthonormal change of basis — guarantees fLocal -> desired exactly.
        const vec3 upProj = *upModel - glm::dot(*upModel, desired) * desired;
        if (glm::length(upProj) >= kEps) {
            const vec3 t0 = desired;
            const vec3 t1 = glm::normalize(upProj);
            const vec3 t2 = glm::cross(t0, t1);
            // Source basis in the LOCAL frame: forward + a deterministic orthogonal up.
            const vec3 s0 = fLocal;
            const vec3 s1 = OrthogonalAxis(fLocal);
            const vec3 s2 = glm::cross(s0, s1);
            const mat3 target(t0, t1, t2);
            const mat3 source(s0, s1, s2);
            desiredGlobal = glm::normalize(glm::quat_cast(target * glm::transpose(source)));
            useUp = true;
        }
    }
    if (!useUp) {
        // No up (or up collinear with the aim): shortest arc, twist left as-is.
        const vec3 curForward = glm::normalize(curGlobal * fLocal);
        desiredGlobal = ShortestArc(curForward, desired) * curGlobal;
    }

    work.deltas[(size_t)joint] = LocalDelta(sk, joint, g, desiredGlobal);
    pose = std::move(work);
    return true;
}

} // namespace forge
