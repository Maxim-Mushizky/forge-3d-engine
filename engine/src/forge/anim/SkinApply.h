#pragma once

namespace forge {

class Mesh;
struct Skeleton;

// Bakes the skeleton's bind pose into the mesh's live vertices (R1: pose ==
// bind). Reads BindVertices()+Skin(), writes MutableVertices(), then
// RecomputeBounds()+UploadVertices() — the same seam sculpting uses, so the
// scene hash / path-tracer refresh ride Mesh::Version() for free. This is the
// only GL-adjacent TU in forge/anim; everything upstream stays unit-testable.
// #147 generalizes this to an arbitrary pose.
void ApplyBindPose(Mesh& mesh, const Skeleton& skeleton);

} // namespace forge
