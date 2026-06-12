add_executable(ForgeEditor
    src/main.cpp
    src/EditorApp.cpp
    src/EditorCamera.cpp
    src/FileDialog.cpp
    src/SculptTool.cpp)

target_link_libraries(ForgeEditor PRIVATE forge imgui imguizmo comdlg32)
