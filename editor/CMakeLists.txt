add_executable(ForgeEditor
    src/main.cpp
    src/EditorApp.cpp
    src/EditorCamera.cpp
    src/FileDialog.cpp
    src/SculptTool.cpp
    src/ExtrudeTool.cpp)

target_include_directories(ForgeEditor PRIVATE vendor ${tinygltf_SOURCE_DIR}) # tinygltf dir: bundled json.hpp
target_link_libraries(ForgeEditor PRIVATE forge imgui imguizmo comdlg32)
