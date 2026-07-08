#pragma once

#include "forge/core/Math.h"

#include <string>
#include <vector>

namespace forge {

// Shared rig asset: joints in topologically sorted order (parents[i] < i), so
// global transforms and the skinning palette are one forward pass. SoA because
// consumers walk one attribute at a time (the palette wants TRS+IBM, the UI
// wants names). Shared between entities the same way Mesh is: entities hold
// shared_ptr copies and never mutate the asset. TRS-only locals: shear (from
// folding non-uniform-scale ancestor chains at import) is unrepresentable and
// gets approximated with a warn — poseable joints need TRS, not matrices.
struct Skeleton {
    std::vector<int> parents;      // -1 = root; guaranteed parents[i] < i
    std::vector<std::string> names;
    std::vector<vec3> bindT;       // local bind translation
    std::vector<quat> bindR;       // local bind rotation
    std::vector<vec3> bindS;       // local bind scale
    std::vector<mat4> inverseBind; // glTF inverseBindMatrices, reordered to match

    size_t JointCount() const { return parents.size(); }
};

// Joint locals -> globals in one forward pass (topo order: a parent is always
// computed before its children). local = T*R*S, matching TransformComponent::
// World() and the importer's NodeLocalMatrix composition. Takes the pose as
// separate t/r/s vectors instead of reading the skeleton's bind fields: this
// is the seam the Pose object plugs into (#147) — bind is just one such pose.
std::vector<mat4> ComputeGlobalTransforms(const Skeleton& skeleton, const std::vector<vec3>& t,
                                          const std::vector<quat>& r, const std::vector<vec3>& s);

// Globals from the skeleton's own bind TRS.
std::vector<mat4> ComputeBindGlobals(const Skeleton& skeleton);

// palette[i] = globals[i] * inverseBind[i] — the per-joint matrix LBS blends.
std::vector<mat4> ComputePalette(const std::vector<mat4>& globals,
                                 const std::vector<mat4>& inverseBind);

// Index of the joint with this name, or -1 if none. Linear scan (rigs are small;
// names come from glTF and are unique per skin). First match wins. #147.
int JointIndex(const Skeleton& skeleton, const std::string& name);

} // namespace forge
