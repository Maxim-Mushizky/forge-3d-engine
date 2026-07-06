#pragma once

#include "forge/core/Math.h"
#include "forge/renderer/Texture2D.h"

#include <memory>
#include <string>

namespace forge {

struct Material {
    vec3 albedo{0.8f, 0.8f, 0.8f}; // tint when a map is present
    float metallic = 0.0f;
    float roughness = 0.6f;
    vec3 emissive{1.0f, 1.0f, 1.0f};
    float emissiveStrength = 0.0f; // radiance multiplier; >0 glows (and emits light in RT)
    float transmission = 0.0f;     // 0 = solid, 1 = clear (water/glass); dielectric only
    float ior = 1.5f;              // index of refraction; water 1.33, glass 1.5, diamond 2.4
    std::shared_ptr<Texture2D> albedoMap;            // sRGB
    std::shared_ptr<Texture2D> metallicRoughnessMap; // linear; glTF packing: G=roughness, B=metallic

    // Rebuildable descriptors ("file:<path>" / "proc:<recipe json>", #113) for
    // the maps above. Empty = no source: either no map, or one that can't be
    // re-derived (glTF-embedded image) — only sourced maps survive a .forge
    // round-trip. Kept next to the pointers so whole-entity undo snapshots
    // carry both together.
    std::string albedoSource;
    std::string mrSource;
};

} // namespace forge
