#include "SkinApply.h"

#include "forge/anim/Skeleton.h"
#include "forge/anim/Skinning.h"
#include "forge/core/Log.h"
#include "forge/renderer/Mesh.h"

namespace forge {

void ApplyBindPose(Mesh& mesh, const Skeleton& skeleton)
{
    if (!mesh.HasSkin()) {
        FORGE_WARN("ApplyBindPose: mesh carries no skin data — skipped");
        return;
    }
    if (skeleton.inverseBind.size() != skeleton.JointCount()) {
        FORGE_WARN("ApplyBindPose: %zu inverse-bind matrices for %zu joints — skipped",
                   skeleton.inverseBind.size(), skeleton.JointCount());
        return;
    }
    std::vector<mat4> palette = ComputePalette(ComputeBindGlobals(skeleton), skeleton.inverseBind);
    // Always deform FROM the bind snapshot: re-skinning already-posed vertices
    // compounds error and loses the rig reference (why Mesh keeps the copy).
    SkinVertices(mesh.BindVertices(), mesh.Skin(), palette, mesh.MutableVertices());
    mesh.RecomputeBounds();
    mesh.UploadVertices();
}

} // namespace forge
