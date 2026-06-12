#pragma once

#include "forge/core/Math.h"
#include "forge/renderer/Texture2D.h"

#include <memory>

namespace forge {

struct Material {
    vec3 albedo{0.8f, 0.8f, 0.8f}; // tint when a map is present
    float metallic = 0.0f;
    float roughness = 0.6f;
    vec3 emissive{1.0f, 1.0f, 1.0f};
    float emissiveStrength = 0.0f; // radiance multiplier; >0 glows (and emits light in RT)
    std::shared_ptr<Texture2D> albedoMap;            // sRGB
    std::shared_ptr<Texture2D> metallicRoughnessMap; // linear; glTF packing: G=roughness, B=metallic
};

} // namespace forge
