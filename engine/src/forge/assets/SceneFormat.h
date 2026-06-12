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
// File layout (version 1):
//   8 bytes  magic "FORGESCN"
//   u32      version
//   u32      json length
//   ...      json header (entities, mesh table, extras)
//   ...      blob section (raw vertex/index data, offsets relative to its start)
// Unknown json keys are ignored on read (forward compatible).

struct SavedMesh {
    // Non-empty recipe = a factory primitive id (e.g. "cube"); vertices/indices
    // stay empty and the editor resolves the shared mesh on load. Empty recipe
    // = unique geometry (sculpted/imported/generated) stored as a raw blob.
    std::string recipe;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct SavedEntity {
    uint64_t id = 0;
    uint64_t parent = 0;
    std::string name;
    vec3 translation{0.0f}, rotation{0.0f}, scale{1.0f};
    int meshIndex = -1; // into SavedScene::meshes; -1 = meshless node (group)

    // Material factors. Texture maps are not persisted in v1 (no source path
    // for glTF-embedded images); the loader warns when they were present.
    vec3 albedo{0.8f};
    float metallic = 0.0f, roughness = 0.5f;
    vec3 emissive{0.0f};
    float emissiveStrength = 0.0f;
    float transmission = 0.0f; // 0 = solid, 1 = clear (water/glass)
    float ior = 1.5f;

    bool lightEnabled = false;
    vec3 lightColor{1.0f};
    float lightIntensity = 0.0f;
    float lightRange = 0.0f;
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

inline constexpr uint32_t kSceneFormatVersion = 1;

} // namespace forge
