#pragma once

#include <string>

namespace forge {

// What the editor should do with a file dropped onto the window (#2).
enum class DropAction {
    ImportModel,    // .gltf / .glb / .obj
    LoadHdri,       // .hdr
    AssignTexture,  // .png / .jpg / .jpeg — albedo onto the entity under the cursor
    OpenScene,      // .forge
    UnsupportedStl, // .stl — recognized so the UI can suggest glTF/OBJ instead
    Unknown,
};

// Routes purely by (case-insensitive) file extension; never touches the disk.
DropAction ClassifyDrop(const std::string& path);

} // namespace forge
