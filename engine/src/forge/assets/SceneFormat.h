#pragma once

#include "forge/core/Math.h"
#include "forge/renderer/Mesh.h" // Vertex layout only — no GL at compile time

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace forge {

// Plain-data snapshot of everything a .forge file stores. Deliberately GL-free:
// EncodeScene/DecodeScene are pure bytes <-> structs and run headless in unit
// tests; SceneSerializer adapts live Scene/Mesh objects to and from this.
//
// File layout (version 3 — v3 added per-vertex skin + per-entity skeleton/pose
// as optional keys, #147; morph targets + per-entity weights are further v3
// optional keys, #149; v2 added submesh ranges + extra material slots, #80;
// v1/v2 files read unchanged, the new keys just default to empty = non-skinned):
//   8 bytes  magic "FORGESCN"
//   u32      version
//   u32      json length
//   ...      json header (entities, mesh table, extras)
//   ...      blob section (raw vertex/index/skin data, offsets relative to its start)
// Unknown json keys are ignored on read (forward compatible).

struct SavedMesh {
    // Non-empty recipe = a factory primitive id (e.g. "cube"); vertices/indices
    // stay empty and the editor resolves the shared mesh on load. Empty recipe
    // = unique geometry (sculpted/imported/generated) stored as a raw blob.
    std::string recipe;
    std::vector<Vertex> vertices; // BIND-pose data for skinned meshes (#147)
    std::vector<uint32_t> indices;
    std::vector<Submesh> submeshes; // empty = single-material (whole buffer, slot 0)
    std::vector<VertexSkin> skin;   // empty = not skinned; parallel to vertices (#147)
    std::vector<MorphTarget> morphTargets; // empty = no morphs; deltas parallel to vertices (#149)
};

// One material slot's factors plus rebuildable texture sources (#113,
// "file:<path>" / "proc:<recipe json>"; empty = none). Maps without a source
// (glTF-embedded images) still don't persist — nothing to re-derive them from.
struct SavedMaterial {
    vec3 albedo{0.8f};
    float metallic = 0.0f, roughness = 0.5f;
    vec3 emissive{0.0f};
    float emissiveStrength = 0.0f;
    float transmission = 0.0f;
    float ior = 1.5f;
    float subsurface = 0.0f; // optional keys, still v2 (#112)
    vec3 subsurfaceColor{0.9f, 0.8f, 0.7f};
    vec3 subsurfaceRadius{0.1f, 0.05f, 0.03f};
    std::string albedoSource;
    std::string mrSource;
};

// Persisted rig (#147). Mirrors forge::Skeleton (SoA). Stored inline per entity in
// JSON (rigs are small); the mesh's per-vertex joint indices reference this ordering.
struct SavedSkeleton {
    std::vector<int> parents;
    std::vector<std::string> names;
    std::vector<vec3> bindT;
    std::vector<quat> bindR;
    std::vector<vec3> bindS;
    std::vector<mat4> inverseBind;

    bool Empty() const { return parents.empty(); }
};

struct SavedEntity {
    uint64_t id = 0;
    uint64_t parent = 0;
    std::string name;
    vec3 translation{0.0f}, rotation{0.0f}, scale{1.0f};
    int meshIndex = -1; // into SavedScene::meshes; -1 = meshless node (group)

    // Material factors. Texture sources persist since #113 (optional keys —
    // still format version 2, older readers just ignore them); sourceless
    // glTF-embedded maps remain unpersistable.
    vec3 albedo{0.8f};
    float metallic = 0.0f, roughness = 0.5f;
    vec3 emissive{0.0f};
    float emissiveStrength = 0.0f;
    float transmission = 0.0f; // 0 = solid, 1 = clear (water/glass)
    float ior = 1.5f;
    float subsurface = 0.0f; // optional keys, still v2 (#112)
    vec3 subsurfaceColor{0.9f, 0.8f, 0.7f};
    vec3 subsurfaceRadius{0.1f, 0.05f, 0.03f};
    std::string albedoSource; // "file:<path>" / "proc:<json>"; empty = none (#113)
    std::string mrSource;
    std::vector<SavedMaterial> extraMaterials; // material slots 1+ (#80); empty pre-v2

    bool lightEnabled = false;
    vec3 lightColor{1.0f};
    float lightIntensity = 0.0f;
    float lightRange = 0.0f;

    SavedSkeleton skeleton;      // empty = not skinned (#147)
    std::vector<quat> pose;      // per-joint local-rotation deltas; empty = bind (#147)
    std::vector<float> morphWeights; // per-instance morph weights; empty = all zero (#149)
};

struct SavedScene {
    std::vector<SavedEntity> entities;
    std::vector<SavedMesh> meshes; // deduplicated; entities reference by index
    std::string extrasJson;        // opaque editor settings (sun, sky, camera, RT)
};

std::vector<uint8_t> EncodeScene(const SavedScene& scene);

// Strict on structure (magic, version, bounds), lenient on content (unknown
// keys ignored, missing fields default). nullopt = unreadable, caller keeps
// the current scene untouched.
std::optional<SavedScene> DecodeScene(const uint8_t* data, size_t size);

inline constexpr uint32_t kSceneFormatVersion = 3; // v3: skin + skeleton + pose (#147)

} // namespace forge
