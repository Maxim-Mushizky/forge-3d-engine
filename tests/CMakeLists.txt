# Unit tests. Deliberately link ONLY glm (header-only) and the engine's include
# dir — NOT the `forge` library — so the suite covers pure logic (geometry math,
# ids, file formats) and builds fast and headless, with no OpenGL/GLFW context
# required in CI. GL-free engine .cpp files are compiled directly into the test
# binary (SceneFormat is bytes<->structs only).
add_executable(forge_tests
    test_main.cpp
    test_geometry.cpp
    test_uuid.cpp
    test_sceneformat.cpp
    test_boxselect.cpp
    test_droptoground.cpp
    test_bvh.cpp
    test_snap.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/assets/SceneFormat.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/raytrace/BVH.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/scene/BoxSelect.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/scene/DropToGround.cpp
    ${CMAKE_SOURCE_DIR}/engine/src/forge/scene/TransformSnap.cpp)

target_include_directories(forge_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/engine/src
    ${tinygltf_SOURCE_DIR}) # bundled nlohmann json.hpp
target_link_libraries(forge_tests PRIVATE glm::glm)

add_test(NAME forge_tests COMMAND forge_tests)
