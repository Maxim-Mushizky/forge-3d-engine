#pragma once

#include "forge/renderer/Material.h"
#include "forge/renderer/Mesh.h"

#include <memory>
#include <string>
#include <vector>

namespace forge {

// One renderable piece of an imported model. Node transforms are baked into
// the vertices, so all parts share a single entity transform on spawn.
// A multi-primitive glTF mesh becomes ONE part (#80): the mesh carries submesh
// ranges, `material` is slot 0 and `extraMaterials` are slots 1+.
struct ImportedPart {
    std::string name;
    std::shared_ptr<Mesh> mesh;
    Material material;
    std::vector<Material> extraMaterials; // material slots 1+ (empty = single-material)
};

class ModelImporter {
public:
    // Supports .gltf, .glb, .obj. Returns empty vector on failure (errors logged).
    static std::vector<ImportedPart> Load(const std::string& path);
};

} // namespace forge
