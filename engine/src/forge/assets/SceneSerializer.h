#pragma once

#include "SceneFormat.h"
#include "forge/scene/Scene.h"

#include <functional>
#include <memory>
#include <string>

namespace forge {

// Editor hooks so shared primitive meshes save by recipe id instead of raw
// blobs (small files, and the shared_ptr stays shared after a load).
// MeshToRecipe returns "" for unique geometry; RecipeToMesh returns null for
// an unknown id (the loader then skips that mesh and warns).
using MeshToRecipe = std::function<std::string(const Mesh*)>;
using RecipeToMesh = std::function<std::shared_ptr<Mesh>(const std::string&)>;

// Live Scene -> plain snapshot (pure, no IO).
SavedScene SnapshotScene(const Scene& scene, const std::string& extrasJson,
                         const MeshToRecipe& toRecipe);

// Snapshot -> live Scene. Replaces outScene's contents. Returns the number of
// entities whose mesh could not be restored (unknown recipe).
int RestoreScene(const SavedScene& saved, Scene& outScene, std::string& outExtrasJson,
                 const RecipeToMesh& fromRecipe);

// File wrappers. Load leaves outScene untouched on failure.
bool SaveSceneFile(const std::string& path, const Scene& scene, const std::string& extrasJson,
                   const MeshToRecipe& toRecipe);
bool LoadSceneFile(const std::string& path, Scene& outScene, std::string& outExtrasJson,
                   const RecipeToMesh& fromRecipe);

} // namespace forge
