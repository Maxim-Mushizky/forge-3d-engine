#include "test_framework.h"

#include <forge/anim/Ik.h>
#include <forge/anim/Pose.h>
#include <forge/anim/Skeleton.h>

#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

// Exact-value coverage for the analytic pose solvers (#148). Chains are built with
// identity bind rotations so the elbow geometry is hand-checkable; one suite adds a
// rotated parent + pre-existing delta to catch the stale-parent-frame bug.

namespace forge::test {

namespace {

bool ApproxVec3(const vec3& a, const vec3& b, float eps = 1e-3f)
{
    return ApproxEq(a.x, b.x, eps) && ApproxEq(a.y, b.y, eps) && ApproxEq(a.z, b.z, eps);
}

bool Finite(const quat& q)
{
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

bool PoseFinite(const Pose& p)
{
    for (const quat& q : p.deltas)
        if (!Finite(q))
            return false;
    return true;
}

// A parent->child chain: parents[i] and per-joint LOCAL translation. Identity bind
// rotation unless overridden. IBMs are the exact inverse of each bind global.
Skeleton MakeChain(const std::vector<int>& parents, const std::vector<vec3>& localT,
                   const std::vector<quat>* bindR = nullptr)
{
    Skeleton sk;
    sk.parents = parents;
    for (size_t i = 0; i < parents.size(); ++i)
        sk.names.push_back("j" + std::to_string(i));
    sk.bindT = localT;
    if (bindR)
        sk.bindR = *bindR;
    else
        sk.bindR.assign(parents.size(), quat(1, 0, 0, 0));
    sk.bindS.assign(parents.size(), vec3(1.0f));
    for (const mat4& g : ComputeBindGlobals(sk))
        sk.inverseBind.push_back(glm::inverse(g));
    return sk;
}

// Model-space joint positions under a pose (the FK the solver targets).
std::vector<vec3> Positions(const Skeleton& sk, const Pose& pose)
{
    std::vector<quat> r = PoseLocalRotations(sk, pose);
    std::vector<mat4> g = ComputeGlobalTransforms(sk, sk.bindT, r, sk.bindS);
    std::vector<vec3> p;
    for (const mat4& m : g)
        p.push_back(vec3(m[3]));
    return p;
}

// Straight 3-joint chain along +Y, unit segments: A=(0,0,0) B=(0,1,0) C=(0,2,0).
Skeleton MakeArm()
{
    return MakeChain({-1, 0, 1}, {vec3(0, 0, 0), vec3(0, 1, 0), vec3(0, 1, 0)});
}

// --- 1. planar 90-degree elbow ------------------------------------------------
void TestPlanarElbow()
{
    Skeleton sk = MakeArm();
    Pose pose;
    const bool ok = SolveTwoBoneIk(sk, pose, {0, 1, 2}, vec3(1, 1, 0), vec3(1, 0, 0));
    CHECK(ok);
    CHECK(PoseFinite(pose));
    std::vector<vec3> p = Positions(sk, pose);
    const vec3 A = p[0], B = p[1], C = p[2];
    CHECK(ApproxEq(glm::length(B - A), 1.0f, 1e-3f)); // bone lengths preserved
    CHECK(ApproxEq(glm::length(C - B), 1.0f, 1e-3f));
    CHECK(ApproxVec3(C, vec3(1, 1, 0)));              // end reaches the target
    CHECK(ApproxEq(glm::dot(glm::normalize(B - A), glm::normalize(C - B)), 0.0f, 1e-3f)); // 90 deg
    CHECK(B.x > 0.0f);                                // elbow on the pole (+X) side
}

// --- 2. overreach clamps to near-full extension --------------------------------
void TestOverreach()
{
    Skeleton sk = MakeArm();
    Pose pose;
    const bool ok = SolveTwoBoneIk(sk, pose, {0, 1, 2}, vec3(0, 10, 0), vec3(1, 0, 0));
    CHECK(ok);
    CHECK(PoseFinite(pose));
    std::vector<vec3> p = Positions(sk, pose);
    // Chain straightens toward the target but stops just shy of full reach (2.0).
    CHECK(ApproxVec3(p[2], vec3(0, 2, 0), 1e-2f));
    CHECK(glm::length(p[2] - vec3(0, 10, 0)) > 1.0f); // "reached" is false: end != target
    CHECK(ApproxEq(glm::dot(glm::normalize(p[1] - p[0]), glm::normalize(p[2] - p[1])), 1.0f, 1e-2f));
}

// --- 3. underreach fold, unequal bones, target on the root ---------------------
void TestUnderreachFold()
{
    Skeleton sk = MakeChain({-1, 0, 1}, {vec3(0, 0, 0), vec3(0, 1.5f, 0), vec3(0, 0.5f, 0)});
    Pose pose;
    const bool ok = SolveTwoBoneIk(sk, pose, {0, 1, 2}, vec3(0, 0, 0), vec3(1, 0, 0));
    CHECK(ok);
    CHECK(PoseFinite(pose));
    std::vector<vec3> p = Positions(sk, pose);
    // Fully folded: end sits |L1-L2| from the root (1.0), not on top of it.
    CHECK(ApproxEq(glm::length(p[2] - p[0]), 1.0f, 1e-2f));
}

// --- 4. pole flip mirrors the elbow -------------------------------------------
void TestPoleFlip()
{
    Skeleton sk = MakeArm();
    Pose a, b;
    CHECK(SolveTwoBoneIk(sk, a, {0, 1, 2}, vec3(1, 1, 0), vec3(1, 0, 0)));
    CHECK(SolveTwoBoneIk(sk, b, {0, 1, 2}, vec3(1, 1, 0), vec3(-1, 0, 0)));
    const vec3 A(0, 0, 0), T(1, 1, 0);
    const vec3 ba = Positions(sk, a)[1];
    const vec3 bb = Positions(sk, b)[1];
    // The elbow bends to the side the pole indicates: cross(T-A, Bnew-A) agrees in
    // sign with cross(T-A, P-A). Holds for both poles (they mirror across the A->T line).
    auto side = [&](const vec3& Bnew, const vec3& P) {
        return glm::dot(glm::cross(T - A, Bnew - A), glm::cross(T - A, P - A));
    };
    CHECK(side(ba, vec3(1, 0, 0)) > 0.0f);
    CHECK(side(bb, vec3(-1, 0, 0)) > 0.0f);
    CHECK(!ApproxVec3(ba, bb, 1e-2f)); // mirrored pole -> distinct elbow
}

// --- 5. the end joint's own delta is never touched ----------------------------
void TestEndDeltaUntouched()
{
    Skeleton sk = MakeArm();
    Pose pose;
    pose.deltas.assign(3, quat(1, 0, 0, 0));
    const quat marker = glm::angleAxis(0.9f, glm::normalize(vec3(0.2f, 0.5f, 0.8f)));
    pose.deltas[2] = marker;
    CHECK(SolveTwoBoneIk(sk, pose, {0, 1, 2}, vec3(1, 1, 0), vec3(1, 0, 0)));
    // Bit-identical: the solver writes only root and mid.
    CHECK(pose.deltas[2].x == marker.x && pose.deltas[2].y == marker.y &&
          pose.deltas[2].z == marker.z && pose.deltas[2].w == marker.w);
}

// --- 6. degenerate pole (collinear with A->T) falls back, no NaN ---------------
void TestDegeneratePole()
{
    Skeleton sk = MakeArm();
    Pose pose;
    // Pole sits on the A->T line, and the current chain is straight -> both plane
    // sources degenerate; the solver must still return a finite pose.
    const bool ok = SolveTwoBoneIk(sk, pose, {0, 1, 2}, vec3(1, 1, 0), vec3(0.5f, 0.5f, 0));
    CHECK(ok);
    CHECK(PoseFinite(pose));
}

// --- 7. FK round-trip under a rotated parent + pre-existing root delta ----------
void TestRotatedParentRoundTrip()
{
    // j0 is a rotated parent above the root; the chain is j1(root)-j2(mid)-j3(end).
    std::vector<quat> bindR(4, quat(1, 0, 0, 0));
    bindR[0] = glm::angleAxis(0.6458f /*~37deg*/, glm::normalize(vec3(0.3f, 0.8f, 0.5f)));
    Skeleton sk = MakeChain({-1, 0, 1, 2},
                            {vec3(0, 0, 0), vec3(0.2f, 0.1f, 0), vec3(0, 1, 0), vec3(0, 1, 0)},
                            &bindR);

    Pose pose;
    pose.deltas.assign(4, quat(1, 0, 0, 0));
    pose.deltas[1] = glm::angleAxis(0.4f, glm::normalize(vec3(1, 0.2f, 0.3f))); // pre-existing root pose

    // Reachable target: 1.4 units from the current root, off-axis so the elbow bends.
    const vec3 A = Positions(sk, pose)[1];
    const vec3 target = A + glm::normalize(vec3(0.6f, 0.7f, 0.4f)) * 1.4f;
    const bool ok = SolveTwoBoneIk(sk, pose, {1, 2, 3}, target, A + vec3(1, 0, 0));
    CHECK(ok);
    CHECK(PoseFinite(pose));
    // Re-run FK from the solved pose: the end lands on the target only if the mid
    // solve used the REFRESHED parent frame (the stale-frame bug would miss here).
    CHECK(ApproxVec3(Positions(sk, pose)[3], target, 2e-3f));
}

// --- 8. aim +X forward onto +Y, with and without an up hint --------------------
void TestAimBasic()
{
    Skeleton sk = MakeChain({-1, 0}, {vec3(0, 0, 0), vec3(1, 0, 0)}); // bone along +X
    const vec3 fLocal(1, 0, 0);
    const vec3 target(0, 2, 0);
    const vec3 desired = glm::normalize(target);

    Pose noUp;
    CHECK(SolveAim(sk, noUp, 0, 1, target, std::nullopt));
    CHECK(PoseFinite(noUp));
    quat r0 = PoseLocalRotations(sk, noUp)[0];
    CHECK(ApproxVec3(glm::normalize(r0 * fLocal), desired));

    Pose withUp, withUp2;
    CHECK(SolveAim(sk, withUp, 0, 1, target, std::optional<vec3>(vec3(0, 0, 1))));
    CHECK(SolveAim(sk, withUp2, 0, 1, target, std::optional<vec3>(vec3(0, 0, 1))));
    CHECK(PoseFinite(withUp));
    quat ru = PoseLocalRotations(sk, withUp)[0];
    CHECK(ApproxVec3(glm::normalize(ru * fLocal), desired)); // forward still hits target
    // Deterministic across repeated calls (same up basis every time).
    quat ru2 = PoseLocalRotations(sk, withUp2)[0];
    CHECK(ru.x == ru2.x && ru.y == ru2.y && ru.z == ru2.z && ru.w == ru2.w);
}

// --- 9. aim antiparallel: target directly behind -------------------------------
void TestAimAntiparallel()
{
    Skeleton sk = MakeChain({-1, 0}, {vec3(0, 0, 0), vec3(1, 0, 0)}); // bone along +X
    const vec3 fLocal(1, 0, 0);
    const vec3 target(-2, 0, 0); // exactly behind
    const vec3 desired = glm::normalize(target);

    Pose a, b;
    CHECK(SolveAim(sk, a, 0, 1, target, std::nullopt));
    CHECK(SolveAim(sk, b, 0, 1, target, std::nullopt));
    CHECK(PoseFinite(a));
    quat ra = PoseLocalRotations(sk, a)[0];
    CHECK(ApproxVec3(glm::normalize(ra * fLocal), desired));
    // Deterministic: same 180-degree axis chosen every call.
    quat rb = PoseLocalRotations(sk, b)[0];
    CHECK(ra.x == rb.x && ra.y == rb.y && ra.z == rb.z && ra.w == rb.w);
}

// --- 10. bad input leaves the pose untouched -----------------------------------
void TestBadInput()
{
    Skeleton sk = MakeArm();
    Pose seed;
    seed.deltas = {glm::angleAxis(0.3f, vec3(0, 0, 1)), glm::angleAxis(0.1f, vec3(1, 0, 0)),
                   quat(1, 0, 0, 0)};

    auto untouched = [&](const Pose& p) {
        if (p.deltas.size() != seed.deltas.size())
            return false;
        for (size_t i = 0; i < p.deltas.size(); ++i)
            if (!(p.deltas[i].x == seed.deltas[i].x && p.deltas[i].w == seed.deltas[i].w))
                return false;
        return true;
    };

    // Index out of range (end == 9).
    Pose p1 = seed;
    CHECK(!SolveTwoBoneIk(sk, p1, {0, 1, 9}, vec3(1, 1, 0), vec3(1, 0, 0)));
    CHECK(untouched(p1));

    // Non-ancestry chain (reversed: 2 is not an ancestor of 1).
    Pose p2 = seed;
    CHECK(!SolveTwoBoneIk(sk, p2, {2, 1, 0}, vec3(1, 1, 0), vec3(1, 0, 0)));
    CHECK(untouched(p2));

    // Zero-length bone: mid co-located with root in bind (L1 == 0).
    Skeleton deg = MakeChain({-1, 0, 1}, {vec3(0, 0, 0), vec3(0, 0, 0), vec3(0, 1, 0)});
    Pose p3 = seed;
    CHECK(!SolveTwoBoneIk(deg, p3, {0, 1, 2}, vec3(1, 1, 0), vec3(1, 0, 0)));
    CHECK(untouched(p3));

    // Aim: forwardChild == joint, and out-of-range joint.
    Pose p4 = seed;
    CHECK(!SolveAim(sk, p4, 1, 1, vec3(0, 2, 0), std::nullopt));
    CHECK(untouched(p4));
    Pose p5 = seed;
    CHECK(!SolveAim(sk, p5, 9, 1, vec3(0, 2, 0), std::nullopt));
    CHECK(untouched(p5));
}

} // namespace

void RunIkTests()
{
    TestPlanarElbow();
    TestOverreach();
    TestUnderreachFold();
    TestPoleFlip();
    TestEndDeltaUntouched();
    TestDegeneratePole();
    TestRotatedParentRoundTrip();
    TestAimBasic();
    TestAimAntiparallel();
    TestBadInput();
}

} // namespace forge::test
