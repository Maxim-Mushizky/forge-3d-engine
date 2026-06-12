#pragma once

#include "forge/renderer/Material.h"
#include "forge/renderer/Mesh.h"

#include <memory>
#include <string>
#include <vector>

namespace forge {

// One renderable piece of an imported model. Node transforms are baked into
// the vertices, so all parts share a single entity transform on spawn.
struct ImportedPart {
    std::string name;
    std::shared_ptr<Mesh> mesh;
    Material material;
};

class ModelImporter {
public:
    // Supports .gltf, .glb, .obj. Returns empty vector on failure (errors logged).
    static std::vector<ImportedPart> Load(const std::string& path);
};

} // namespace forge
