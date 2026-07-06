#include "TextureSource.h"

#include "forge/core/Log.h"
#include "forge/renderer/TextureGen.h"

namespace forge {

namespace {
constexpr const char kFilePrefix[] = "file:";
constexpr const char kProcPrefix[] = "proc:";

bool HasPrefix(const std::string& s, const char* prefix)
{
    return s.rfind(prefix, 0) == 0;
}
} // namespace

std::shared_ptr<Texture2D> TextureFromSource(const std::string& source, TextureChannel channel)
{
    const bool srgb = channel == TextureChannel::Albedo;

    if (HasPrefix(source, kFilePrefix)) {
        const std::string path = source.substr(sizeof(kFilePrefix) - 1);
        if (channel == TextureChannel::Albedo)
            return Texture2D::FromFile(path, /*srgb=*/true, /*flipV=*/false);
        // Roughness files carry the value in luminance; repack glTF-style
        // (G = roughness, B = 255) so the existing MR shader paths apply.
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        if (!Texture2D::LoadPixels(path, rgba, w, h))
            return nullptr;
        PackLuminanceToMR(rgba);
        return std::make_shared<Texture2D>(rgba.data(), w, h, 4, /*srgb=*/false);
    }

    if (HasPrefix(source, kProcPrefix)) {
        auto recipe = RecipeFromJsonText(source.substr(sizeof(kProcPrefix) - 1));
        if (!recipe) {
            FORGE_ERROR("Invalid procedural texture recipe: %s", source.c_str());
            return nullptr;
        }
        std::vector<uint8_t> rgba = BakeTexture(*recipe, /*encodeSrgb=*/srgb);
        if (channel == TextureChannel::Roughness)
            PackLuminanceToMR(rgba);
        return std::make_shared<Texture2D>(rgba.data(), recipe->resolution, recipe->resolution, 4,
                                           srgb);
    }

    FORGE_ERROR("Unknown texture source (want file:<path> or proc:<json>): %s", source.c_str());
    return nullptr;
}

} // namespace forge
