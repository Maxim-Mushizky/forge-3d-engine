#pragma once

#include "forge/renderer/Texture2D.h"

#include <memory>
#include <string>

namespace forge {

// Builds GPU textures from persistable source descriptors (#113):
//   "file:<path>"        — image loaded from disk
//   "proc:<recipe json>" — procedural recipe baked by TextureGen
// The descriptor string is what Material stores and .forge files persist, so
// every consumer (set_texture tool, set_material, scene load) rebuilds
// textures through this one function and gets identical results.

enum class TextureChannel {
    Albedo,   // sRGB texture, pixels used as-is
    Roughness // linear texture, luminance packed into the MR map's G channel
};

// nullptr on failure (bad prefix, unreadable file, invalid recipe) after
// logging the reason. Requires a current GL context.
std::shared_ptr<Texture2D> TextureFromSource(const std::string& source, TextureChannel channel);

} // namespace forge
