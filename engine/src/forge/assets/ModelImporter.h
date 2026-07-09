#pragma once

#include "forge/anim/Skeleton.h"
#include "forge/renderer/Material.h"
#include "forge/renderer/Mesh.h"

#include <memory>
#include <string>
#include <vector>

namespace forge {

// One renderable piece of an imported model. Node transforms are baked into
// the vertices, so all parts share a single entity transform on spawn —
// EXCEPT skinned parts (#146), whose vertices stay raw because glTF ignores
// the node transform for skinned meshes (joint globals supply all placement).
// A multi-primitive glTF mesh becomes ONE part (#80): the mesh carries submesh
// ranges, `material` is slot 0 and `extraMaterials` are slots 1+.
struct ImportedPart {
    std::string name;
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<Skeleton> skeleton; // null = rigid part; shared by all parts of one glTF skin
    Material material;
    std::vector<Material> extraMaterials; // material slots 1+ (empty = single-material)
    // Initial morph weights (#149), glTF precedence: node.weights, else mesh.weights,
    // else zeros. Sized to the mesh's target count; empty = no morph targets.
    std::vector<float> defaultMorphWeights;
};

class ModelImporter {
public:
    // Supports .gltf, .glb, .obj. Returns empty vector on failure (errors logged).
    static std::vector<ImportedPart> Load(const std::string& path);
};

} // namespace forge
