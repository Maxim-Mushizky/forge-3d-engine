#pragma once

#include "forge/core/Math.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace forge {

// GL-free procedural texture baker (#113). Recipes bake CPU-side into RGBA8
// buffers so the raster path and the path tracer sample the exact same texels,
// and so a recipe (not megabytes of pixels) is what a .forge file persists.
// Bakes are deterministic for a fixed recipe: the noise is a hashed integer
// lattice, never rand().

enum class TextureKind { Checker, Stripes, Gradient, Noise, Wood };

struct TextureRecipe {
    TextureKind kind = TextureKind::Checker;
    uint32_t resolution = 512;     // square texel size; parser clamps to [16, 4096]
    uint32_t seed = 0;             // varies noise/wood; other kinds ignore it
    vec3 colorA{0.9f, 0.9f, 0.9f}; // linear 0-1
    vec3 colorB{0.1f, 0.1f, 0.1f};
    float scale = 4.0f;   // checker cells / stripe pairs / noise cells / wood rings
    int octaves = 4;      // fbm detail for noise + wood grain, clamped 1-8
    float distort = 0.2f; // wood: ring distortion amplitude ("grainNoise")
    float ratio = 0.5f;   // stripes: fraction of each period covered by colorA
    int axis = 0;         // stripes/gradient direction: 0 = u, 1 = v
};

// Parses {kind, resolution, seed, colorA, colorB, scale, octaves, distort,
// ratio, axis} with the issue-spec aliases ringScale -> scale and
// grainNoise -> distort. Out-of-range values clamp; a missing or unknown kind
// rejects (nullopt) so a typo can't silently bake a checker.
std::optional<TextureRecipe> RecipeFromJsonText(const std::string& jsonText);

// Canonical form of a recipe (aliases resolved, values clamped) — this is the
// string persisted in Material sources, so FromJsonText(ToJsonText(r)) == r.
std::string RecipeToJsonText(const TextureRecipe& recipe);

// RGBA8, resolution^2 * 4 bytes, row 0 = v0, alpha 255, 2x2 supersampled so
// hard checker/stripe edges don't shimmer in the mip-less path tracer.
// encodeSrgb: apply linear->sRGB before quantizing — use it for albedo bakes
// stored in sRGB textures so GPU decode returns the recipe's linear colors.
// Checker, stripes and noise tile exactly (integer cell/period counts);
// gradient and wood do not promise tileability.
std::vector<uint8_t> BakeTexture(const TextureRecipe& recipe, bool encodeSrgb);

// Converts an RGBA image into a glTF-packed metallic-roughness map in place:
// G = luminance(RGB) as roughness, B = 255 and R/A = 255 so the material's
// metallic factor passes through unchanged (shaders multiply factors by G/B).
void PackLuminanceToMR(std::vector<uint8_t>& rgba);

// Bilinear resample (clamped edges). Used to fit arbitrary-sized textures into
// the path tracer's fixed-size texture-array layers.
std::vector<uint8_t> ResampleRGBA(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                                  uint32_t dstW, uint32_t dstH);

} // namespace forge
