#include "SkinApply.h"

#include "forge/anim/Morph.h"
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
    // No entity in scope here, so no live morph weights: skin-only. Call sites
    // that hold the Entity use ApplyDeform with its morphWeights instead (#149).
    ApplyDeform(mesh, &skeleton, &pose, {});
}

void ApplyDeform(Mesh& mesh, const Skeleton* skeleton, const Pose* pose,
                 const std::vector<float>& morphWeights)
{
    const bool skinned = skeleton && mesh.HasSkin();
    const bool morphed = mesh.HasMorphTargets();
    if (!skinned && !morphed) {
        FORGE_WARN("ApplyDeform: mesh carries neither skin nor morph targets — skipped");
        return;
    }
    if (skinned && skeleton->inverseBind.size() != skeleton->JointCount()) {
        FORGE_WARN("ApplyDeform: %zu inverse-bind matrices for %zu joints — skipped",
                   skeleton->inverseBind.size(), skeleton->JointCount());
        return;
    }

    // Morph stage (glTF normative: displacements apply BEFORE skinning). All-zero
    // weights short-circuit to the raw bind snapshot so the common non-morphing
    // path stays a straight copy.
    bool anyWeight = false;
    for (float w : morphWeights)
        if (w != 0.0f) {
            anyWeight = true;
            break;
        }
    // Always deform FROM the bind snapshot (see Mesh::SetSkin): re-deforming
    // already-posed vertices compounds error and loses the rig reference.
    const std::vector<Vertex>* base = &mesh.BindVertices();
    std::vector<Vertex> morphedBind;
    if (morphed && anyWeight) {
        MorphVertices(mesh.BindVertices(), mesh.MorphTargets(), morphWeights, morphedBind);
        base = &morphedBind;
    }

    if (skinned) {
        // bind T/S unchanged; only the per-joint rotation is overridden by the pose.
        std::vector<quat> r = PoseLocalRotations(*skeleton, pose ? *pose : Pose{});
        std::vector<mat4> globals =
            ComputeGlobalTransforms(*skeleton, skeleton->bindT, r, skeleton->bindS);
        std::vector<mat4> palette = ComputePalette(globals, skeleton->inverseBind);
        // SkinVertices renormalizes the (morphed) normals of every weighted vertex.
        SkinVertices(*base, mesh.Skin(), palette, mesh.MutableVertices());
    } else {
        mesh.MutableVertices() = *base;
        // MorphVertices keeps the pure spec sum; the morph-only path is the final
        // consumer, so normalize here (degenerate sums keep the summed direction
        // out of the VBO — mirror SkinVertices' guard by keeping the bind normal).
        if (anyWeight) {
            std::vector<Vertex>& live = mesh.MutableVertices();
            for (size_t v = 0; v < live.size(); ++v) {
                const float len = glm::length(live[v].normal);
                live[v].normal =
                    len > 1e-8f ? live[v].normal / len : mesh.BindVertices()[v].normal;
            }
        }
    }
    mesh.RecomputeBounds();
    mesh.UploadVertices();
}

void ApplyBindPose(Mesh& mesh, const Skeleton& skeleton)
{
    ApplyPose(mesh, skeleton, Pose{}); // bind == the empty pose
}

std::shared_ptr<Mesh> CloneSkinnedMesh(const Mesh& mesh)
{
    if (!mesh.HasSkin() && !mesh.HasMorphTargets())
        return nullptr;
    auto clone = std::make_shared<Mesh>(mesh.BindVertices(), mesh.Indices(), mesh.Submeshes());
    if (mesh.HasSkin())
        clone->SetSkin(mesh.Skin()); // snapshots the bind verts we just set as the bind reference
    if (mesh.HasMorphTargets())
        clone->SetMorphTargets(mesh.MorphTargets()); // snapshots for morph-only meshes
    return clone; // undeformed; caller applies a pose / weights
}

} // namespace forge
