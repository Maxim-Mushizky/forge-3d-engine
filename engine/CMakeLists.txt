add_library(forge STATIC
    src/forge/platform/Window.cpp
    src/forge/renderer/Shader.cpp
    src/forge/renderer/Mesh.cpp
    src/forge/renderer/Texture2D.cpp
    src/forge/renderer/Environment.cpp
    src/forge/renderer/Framebuffer.cpp
    src/forge/renderer/PostProcess.cpp
    src/forge/renderer/Renderer.cpp
    src/forge/scene/Scene.cpp
    src/forge/assets/MeshFactory.cpp
    src/forge/assets/ModelImporter.cpp
    src/forge/assets/StlExporter.cpp
    src/forge/assets/SceneFormat.cpp
    src/forge/assets/SceneSerializer.cpp
    src/forge/scene/BoxSelect.cpp
    src/forge/scene/DropToGround.cpp
    src/forge/scene/TransformSnap.cpp
    src/forge/geometry/EditMesh.cpp
    src/forge/geometry/MeshEdit.cpp
    src/forge/geometry/MeshBoolean.cpp
    src/forge/geometry/MeshRemesh.cpp
    src/forge/raytrace/BVH.cpp
    src/forge/raytrace/PathTracer.cpp)

target_include_directories(forge PUBLIC src)
target_include_directories(forge PRIVATE ${tinygltf_SOURCE_DIR} ${tinyobjloader_SOURCE_DIR} vendor)
target_link_libraries(forge PUBLIC glfw libglew_static glm::glm opengl32)
target_link_libraries(forge PRIVATE manifold::manifold)
# Where the editor loads runtime assets (shaders, models) from. Defaults to the
# in-tree assets dir for local dev; override with -D FORGE_ASSET_DIR="assets" to
# resolve relative to the working dir for a portable deployment bundle.
set(FORGE_ASSET_DIR "${CMAKE_SOURCE_DIR}/assets" CACHE STRING "Runtime asset directory")
target_compile_definitions(forge PUBLIC FORGE_ASSET_DIR="${FORGE_ASSET_DIR}")
