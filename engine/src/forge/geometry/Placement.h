#pragma once

#include "forge/core/Geometry.h"
#include "forge/core/Math.h"

#include <vector>

namespace forge {

// GL-free relational-placement solver (#95). LLMs emit bad absolute
// coordinates; these kernels turn declarative relations ("on", "against",
// "around") into world-space translation deltas solved from AABBs, so the
// agent never computes a coordinate. All functions take world-space boxes
// (rotation is already folded in by TransformAABB upstream) and return deltas
// to ADD to a world position; callers convert to parent space.

// Rest `entity` on top of `anchor`: bottom face at anchor.max.y + clearance.
// XZ: recentred over the anchor unless the entity's centre is already inside
// the anchor's footprint (a lamp nudged to a desk corner should stay there).
vec3 SolveOn(const AABB& entity, const AABB& anchor, float clearance);

// Side of `anchor` the entity currently sits toward: 0 = +x, 1 = -x,
// 2 = +z, 3 = -z (largest XZ centre offset wins; ties prefer x).
int NearestSide(const AABB& entity, const AABB& anchor);

// Abut `entity` against the given side of `anchor` with a `clearance` gap,
// centred along the wall's run, bottoms aligned (furniture against a wall
// stands on the wall's floor, not at its own old height).
vec3 SolveAgainst(const AABB& entity, const AABB& anchor, int side, float clearance);

// Yaw (radians, about +y) that points local +z from `from` toward `to`.
// Zero when the target is straight down +z, matching the scene's facing
// convention (front = +z side).
float YawToward(const vec3& from, const vec3& to);

struct PlacedPose {
    vec3 delta{0.0f};   // world translation to add
    float yawRad = 0.0f; // absolute yaw about +y (face the anchor)
};

// Ring of `count` poses around `anchor` for copies of `entity`: radius is the
// two footprints' half-diagonals plus clearance (half-diagonals guarantee no
// corner clip at any angle), bottoms aligned with the anchor's, every pose
// yawed to face the anchor centre. Slot 0 sits at -z (in front), then
// counter-clockwise. count <= 0 returns empty.
std::vector<PlacedPose> SolveAround(const AABB& entity, const AABB& anchor, int count,
                                    float clearance);

// Push `moving` out of every overlapping obstacle by repeated minimum-
// translation-vector steps. Returns the accumulated extra delta; gives up
// after maxIterations (crowded scenes can oscillate — the caller reports the
// residual overlap rather than looping forever).
vec3 NudgeOut(const AABB& moving, const std::vector<AABB>& obstacles, int maxIterations = 8);

enum class AlignMode { Min = 0, Center = 1, Max = 2 };

// Per-box delta along `axis` (0=x,1=y,2=z) that brings the chosen feature
// (min face / centre / max face) to `target`. Invalid boxes get delta 0.
std::vector<float> SolveAlign(const std::vector<AABB>& boxes, int axis, AlignMode mode,
                              float target);

// Per-box delta along `axis` spacing the boxes out in their current order
// along that axis. spacing >= 0: first box (by centre) stays put and each
// next box's min face sits at the previous max + spacing. spacing < 0:
// centres spread evenly between the current first and last centres (classic
// "distribute"). Fewer than 2 valid boxes -> all-zero deltas.
std::vector<float> SolveDistribute(const std::vector<AABB>& boxes, int axis, float spacing);

} // namespace forge
