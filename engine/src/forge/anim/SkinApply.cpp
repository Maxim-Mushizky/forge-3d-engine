#include "SkinApply.h"

#include "forge/anim/Pose.h"
#include "forge/anim/Skeleton.h"
#include "forge/anim/Skinning.h"
#include "forge/core/Log.h"
#include "forge/renderer/Mesh.h"

namespace forge {

void ApplyPose(Mesh& mesh, const Skeleton& skeleton, const Pose& pose)
{
    if (!mesh.HasSkin()) {
        FORGE_WARN("ApplyPose: mesh carries no skin data — skipped");
        return;
    }
    if (skeleton.inverseBind.size() != skeleton.JointCount()) {
        FORGE_WARN("ApplyPose: %zu inverse-bind matrices for %zu joints — skipped",
                   skeleton.inverseBind.size(), skeleton.JointCount());
        return;
    }
    // bind T/S unchanged; only the per-joint rotation is overridden by the pose.
    std::vector<quat> r = PoseLocalRotations(skeleton, pose);
    std::vector<mat4> globals = ComputeGlobalTransforms(skeleton, skeleton.bindT, r, skeleton.bindS);
    std::vector<mat4> palette = ComputePalette(globals, skeleton.inverseBind);
    // Always deform FROM the bind snapshot (see Mesh::SetSkin): re-skinning already-
    // posed vertices compounds error and loses the rig reference.
    SkinVertices(mesh.BindVertices(), mesh.Skin(), palette, mesh.MutableVertices());
    mesh.RecomputeBounds();
    mesh.UploadVertices();
}

void ApplyBindPose(Mesh& mesh, const Skeleton& skeleton)
{
    ApplyPose(mesh, skeleton, Pose{}); // bind == the empty pose
}

std::shared_ptr<Mesh> CloneSkinnedMesh(const Mesh& mesh)
{
    if (!mesh.HasSkin())
        return nullptr;
    auto clone = std::make_shared<Mesh>(mesh.BindVertices(), mesh.Indices(), mesh.Submeshes());
    clone->SetSkin(mesh.Skin()); // snapshots the bind verts we just set as the bind reference
    return clone;                // undeformed; caller applies a pose
}

} // namespace forge
