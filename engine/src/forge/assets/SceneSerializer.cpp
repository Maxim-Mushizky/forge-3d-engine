#include "SceneSerializer.h"

#include "forge/core/Log.h"
#include "forge/renderer/TextureSource.h"

#include <fstream>
#include <unordered_map>

namespace forge {

SavedScene SnapshotScene(const Scene& scene, const std::string& extrasJson,
                         const MeshToRecipe& toRecipe)
{
    SavedScene out;
    out.extrasJson = extrasJson;

    std::unordered_map<const Mesh*, int> meshIndex; // dedupe shared meshes
    for (const Entity& e : scene.Entities()) {
        SavedEntity se;
        se.id = e.id;
        se.parent = e.parent;
        se.name = e.name;
        se.translation = e.transform.translation;
        se.rotation = e.transform.rotation;
        se.scale = e.transform.scale;
        se.albedo = e.material.albedo;
        se.metallic = e.material.metallic;
        se.roughness = e.material.roughness;
        se.emissive = e.material.emissive;
        se.emissiveStrength = e.material.emissiveStrength;
        se.transmission = e.material.transmission;
        se.ior = e.material.ior;
        se.albedoSource = e.material.albedoSource;
        se.mrSource = e.material.mrSource;
        for (const Material& m : e.extraMaterials) {
            SavedMaterial sm;
            sm.albedo = m.albedo;
            sm.metallic = m.metallic;
            sm.roughness = m.roughness;
            sm.emissive = m.emissive;
            sm.emissiveStrength = m.emissiveStrength;
            sm.transmission = m.transmission;
            sm.ior = m.ior;
            sm.albedoSource = m.albedoSource;
            sm.mrSource = m.mrSource;
            se.extraMaterials.push_back(sm);
        }
        se.lightEnabled = e.light.enabled;
        se.lightColor = e.light.color;
        se.lightIntensity = e.light.intensity;
        se.lightRange = e.light.range;

        if (e.mesh) {
            auto [it, inserted] = meshIndex.try_emplace(e.mesh.get(), (int)out.meshes.size());
            if (inserted) {
                SavedMesh sm;
                sm.recipe = toRecipe ? toRecipe(e.mesh.get()) : std::string();
                if (sm.recipe.empty()) {
                    sm.vertices = e.mesh->Vertices();
                    sm.indices = e.mesh->Indices();
                    sm.submeshes = e.mesh->Submeshes();
                }
                out.meshes.push_back(std::move(sm));
            }
            se.meshIndex = it->second;
        }
        out.entities.push_back(std::move(se));
    }
    return out;
}

int RestoreScene(const SavedScene& saved, Scene& outScene, std::string& outExtrasJson,
                 const RecipeToMesh& fromRecipe)
{
    outScene.Entities().clear();
    outExtrasJson = saved.extrasJson;

    // Meshes first; entities reference them by index.
    std::vector<std::shared_ptr<Mesh>> meshes;
    meshes.reserve(saved.meshes.size());
    int missing = 0;
    for (const SavedMesh& sm : saved.meshes) {
        if (!sm.recipe.empty()) {
            std::shared_ptr<Mesh> m = fromRecipe ? fromRecipe(sm.recipe) : nullptr;
            if (!m)
                FORGE_WARN("Scene load: unknown primitive recipe '%s'", sm.recipe.c_str());
            meshes.push_back(std::move(m));
        } else if (!sm.vertices.empty() && !sm.indices.empty()) {
            meshes.push_back(std::make_shared<Mesh>(sm.vertices, sm.indices, sm.submeshes));
        } else {
            meshes.push_back(nullptr);
        }
    }

    // Rebuild textures from their persisted sources (#113). A failed rebuild
    // (moved file, bad recipe) degrades to factors-only with a warning — same
    // leniency as missing mesh recipes; the source string is kept so a later
    // re-save doesn't destroy the reference.
    auto rebuildTextures = [](Material& m) {
        if (!m.albedoSource.empty()) {
            m.albedoMap = TextureFromSource(m.albedoSource, TextureChannel::Albedo);
            if (!m.albedoMap)
                FORGE_WARN("Scene load: albedo texture source failed: %s", m.albedoSource.c_str());
        }
        if (!m.mrSource.empty()) {
            m.metallicRoughnessMap = TextureFromSource(m.mrSource, TextureChannel::Roughness);
            if (!m.metallicRoughnessMap)
                FORGE_WARN("Scene load: roughness texture source failed: %s", m.mrSource.c_str());
        }
    };

    for (const SavedEntity& se : saved.entities) {
        Entity e;
        e.id = se.id ? se.id : GenerateUUID(); // tolerate hand-edited files without ids
        e.parent = se.parent;
        e.name = se.name;
        e.transform.translation = se.translation;
        e.transform.rotation = se.rotation;
        e.transform.scale = se.scale;
        e.material.albedo = se.albedo;
        e.material.metallic = se.metallic;
        e.material.roughness = se.roughness;
        e.material.emissive = se.emissive;
        e.material.emissiveStrength = se.emissiveStrength;
        e.material.transmission = se.transmission;
        e.material.ior = se.ior;
        e.material.albedoSource = se.albedoSource;
        e.material.mrSource = se.mrSource;
        rebuildTextures(e.material);
        for (const SavedMaterial& sm : se.extraMaterials) {
            Material m;
            m.albedo = sm.albedo;
            m.metallic = sm.metallic;
            m.roughness = sm.roughness;
            m.emissive = sm.emissive;
            m.emissiveStrength = sm.emissiveStrength;
            m.transmission = sm.transmission;
            m.ior = sm.ior;
            m.albedoSource = sm.albedoSource;
            m.mrSource = sm.mrSource;
            rebuildTextures(m);
            e.extraMaterials.push_back(m);
        }
        e.light.enabled = se.lightEnabled;
        if (se.lightEnabled) {
            e.light.color = se.lightColor;
            e.light.intensity = se.lightIntensity;
            e.light.range = se.lightRange;
        }
        if (se.meshIndex >= 0 && se.meshIndex < (int)meshes.size()) {
            e.mesh = meshes[se.meshIndex];
            if (!e.mesh)
                ++missing;
        }
        outScene.Insert(e);
    }
    return missing;
}

bool SaveSceneFile(const std::string& path, const Scene& scene, const std::string& extrasJson,
                   const MeshToRecipe& toRecipe)
{
    std::vector<uint8_t> bytes = EncodeScene(SnapshotScene(scene, extrasJson, toRecipe));

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
    out.close();
    if (!out)
        return false;
    FORGE_INFO("Scene saved: %s (%zu bytes)", path.c_str(), bytes.size());
    return true;
}

bool LoadSceneFile(const std::string& path, Scene& outScene, std::string& outExtrasJson,
                   const RecipeToMesh& fromRecipe)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    std::vector<uint8_t> bytes((size_t)in.tellg());
    in.seekg(0);
    in.read((char*)bytes.data(), (std::streamsize)bytes.size());
    if (!in)
        return false;

    std::optional<SavedScene> saved = DecodeScene(bytes.data(), bytes.size());
    if (!saved) {
        FORGE_ERROR("Scene load failed (corrupt or newer-version file): %s", path.c_str());
        return false;
    }
    int missing = RestoreScene(*saved, outScene, outExtrasJson, fromRecipe);
    FORGE_INFO("Scene loaded: %s (%zu entities%s)", path.c_str(), outScene.Entities().size(),
               missing ? ", some meshes missing" : "");
    return true;
}

} // namespace forge
