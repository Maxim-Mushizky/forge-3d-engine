add_library(forge STATIC
    src/forge/platform/Window.cpp
    src/forge/renderer/Shader.cpp
    src/forge/renderer/Mesh.cpp
    src/forge/renderer/Texture2D.cpp
    src/forge/renderer/TextureGen.cpp
    src/forge/renderer/TextureSource.cpp
    src/forge/renderer/Environment.cpp
    src/forge/renderer/Framebuffer.cpp
    src/forge/renderer/PostProcess.cpp
    src/forge/renderer/Renderer.cpp
    src/forge/scene/Scene.cpp
    src/forge/assets/MeshBuild.cpp
    src/forge/assets/MeshFactory.cpp
    src/forge/assets/ModelImporter.cpp
    src/forge/assets/StlExporter.cpp
    src/forge/assets/SceneFormat.cpp
    src/forge/assets/SceneSerializer.cpp
    src/forge/scene/BoxSelect.cpp
    src/forge/scene/DropToGround.cpp
    src/forge/scene/TransformSnap.cpp
    src/forge/anim/Skeleton.cpp
    src/forge/anim/Skinning.cpp
    src/forge/anim/SkinImport.cpp
    src/forge/anim/SkinApply.cpp
    src/forge/anim/Pose.cpp
    src/forge/anim/PosePresets.cpp
    src/forge/anim/Ik.cpp
    src/forge/geometry/EditMesh.cpp
    src/forge/geometry/MeshEdit.cpp
    src/forge/geometry/MeshBoolean.cpp
    src/forge/geometry/MeshRemesh.cpp
    src/forge/geometry/MeshStats.cpp
    src/forge/geometry/UvUnwrap.cpp
    src/forge/geometry/Spatial.cpp
    src/forge/geometry/Silhouette.cpp
    src/forge/geometry/Placement.cpp
    src/forge/raytrace/BVH.cpp
    src/forge/raytrace/PathTracer.cpp
    src/forge/raytrace/Sss.cpp)

target_include_directories(forge PUBLIC src)
target_include_directories(forge PRIVATE ${tinygltf_SOURCE_DIR} ${tinyobjloader_SOURCE_DIR} vendor)
target_link_libraries(forge PUBLIC glfw libglew_static glm::glm opengl32)
target_link_libraries(forge PRIVATE manifold::manifold xatlas)
# Where the editor loads runtime assets (shaders, models) from. Defaults to the
# in-tree assets dir for local dev; override with -D FORGE_ASSET_DIR="assets" to
# resolve relative to the working dir for a portable deployment bundle.
set(FORGE_ASSET_DIR "${CMAKE_SOURCE_DIR}/assets" CACHE STRING "Runtime asset directory")
target_compile_definitions(forge PUBLIC FORGE_ASSET_DIR="${FORGE_ASSET_DIR}")
