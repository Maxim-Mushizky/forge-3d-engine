#include "DropRouter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace forge {

DropAction ClassifyDrop(const std::string& path)
{
    // filesystem::path handles "dot in a directory name" and "no extension"
    // correctly; extension() includes the leading '.'.
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    if (ext == ".gltf" || ext == ".glb" || ext == ".obj")
        return DropAction::ImportModel;
    if (ext == ".hdr")
        return DropAction::LoadHdri;
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg")
        return DropAction::AssignTexture;
    if (ext == ".forge")
        return DropAction::OpenScene;
    if (ext == ".stl")
        return DropAction::UnsupportedStl;
    return DropAction::Unknown;
}

} // namespace forge
