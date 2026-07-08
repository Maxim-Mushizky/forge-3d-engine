#pragma once

#include <memory>

namespace forge {

class Mesh;
struct Pose;
struct Skeleton;

// Bakes the skeleton's bind pose into the mesh's live vertices (R1: pose ==
// bind). Reads BindVertices()+Skin(), writes MutableVertices(), then
// RecomputeBounds()+UploadVertices() — the same seam sculpting uses, so the
// scene hash / path-tracer refresh ride Mesh::Version() for free. This is the
// only GL-adjacent TU in forge/anim; everything upstream stays unit-testable.
// Since #147 this is just ApplyPose with the empty (= bind) pose.
void ApplyBindPose(Mesh& mesh, const Skeleton& skeleton);

// Deforms the mesh's live vertices from its BIND snapshot using a runtime pose
// (per-joint local-rotation deltas). Re-skins from BindVertices() every call, never
// compounding — same invariant ApplyBindPose relies on. Then RecomputeBounds() +
// UploadVertices(), so the scene hash / path-tracer refresh ride Mesh::Version() as
// usual. GL-adjacent (the only such TU in forge/anim). #147.
void ApplyPose(Mesh& mesh, const Skeleton& skeleton, const Pose& pose);

// A private, pose-ready copy of a skinned mesh: rebuilds a fresh Mesh from the BIND
// vertices (NOT the current, possibly-deformed ones), re-attaches the skin (which
// snapshots those bind verts as the new bind reference), and returns it undeformed.
// Callers deform it with ApplyPose. Needed because Mesh is non-copyable and because
// deforming a mesh shared with a sibling entity or an undo snapshot must copy-on-write
// from bind, not from the posed state. Returns nullptr if the mesh carries no skin. #147.
std::shared_ptr<Mesh> CloneSkinnedMesh(const Mesh& mesh);

} // namespace forge
