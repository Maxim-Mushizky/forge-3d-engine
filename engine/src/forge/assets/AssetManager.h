#pragma once

#include "forge/assets/ModelImporter.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace forge {

// Path-keyed cache so re-importing the same file reuses GPU buffers and textures.
class AssetManager {
public:
    static AssetManager& Get()
    {
        static AssetManager instance;
        return instance;
    }

    // nullptr on failure. Failures are not cached so a fixed file can be retried.
    const std::vector<ImportedPart>* LoadModel(const std::string& path)
    {
        if (auto it = m_Models.find(path); it != m_Models.end())
            return &it->second;
        std::vector<ImportedPart> parts = ModelImporter::Load(path);
        if (parts.empty())
            return nullptr;
        return &(m_Models[path] = std::move(parts));
    }

private:
    std::unordered_map<std::string, std::vector<ImportedPart>> m_Models;
};

} // namespace forge
