# Unit tests. Deliberately link ONLY glm (header-only) and the Lua interpreter
# core (#78, equally headless) — NOT the `forge` library — so the suite covers
# pure logic (geometry math, ids, file formats) and builds fast with no
# OpenGL/GLFW context required in CI. GL-free engine .cpp files are compiled
# directly into the test binary (SceneFormat is bytes<->structs only).
add_executable(forge_tests
    test_main.cpp
    test_geometry.cpp
    test_uuid.cpp
    test_sceneformat.cpp
    test_boxselect.cpp
    test_droptoground.cpp
    test_bvh.cpp
    test_snap.cpp
    test_editmesh.cpp
    test_droprouter.cpp
    test_settings.cpp
    test_mcp.cpp
    test_mcpscript.cpp
    test_mcpviews.cpp
    test_meshstats.cpp
    test_meshbuild.cpp
    test_spatial.cpp
    test_placement.cpp
    test_mcpelements.cpp
    test_mcpsculpt.cpp
    ${CMAKE_SOURCE_DIR}/editor/src/DropRouter.cpp
    ${CMAKE_SOURCE_DIR}/editor/src/Settings.cpp
    ${CMAKE_SOURCE_DIR}/editor/src/mcp/McpProtocol.cpp
    ${CMAKE_SOURCE_DIR}/editor/src/mcp/McpScript.cpp
    ${CMAKE_SOURCE_DIR}/editor/src/mcp/McpViews.cpp
    ${CMAKE_SOURCE_DIR}/editor/src/mcp/McpElements.cpp
    ${CMAKE_SOURCE_DIR}/editor/src/mcp/McpSculpt.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/geometry/EditMesh.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/geometry/MeshStats.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/geometry/Spatial.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/geometry/Placement.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/assets/SceneFormat.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/assets/MeshBuild.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/raytrace/BVH.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/scene/BoxSelect.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/scene/DropToGround.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/scene/TransformSnap.cpp)

target_include_directories(forge_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src
    ${CMAKE_SOURCE_DIR}/editor/src # GL-free editor kernels (DropRouter)
    ${tinygltf_SOURCE_DIR}) # bundled nlohmann json.hpp
target_link_libraries(forge_tests PRIVATE glm::glm lua)

add_test(NAME forge_tests COMMAND forge_tests)
