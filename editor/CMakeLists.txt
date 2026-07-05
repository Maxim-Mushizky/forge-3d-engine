add_executable(ForgeEditor
    src/main.cpp
    src/EditorApp.cpp
    src/EditorCamera.cpp
    src/FileDialog.cpp
    src/SculptTool.cpp
    src/ExtrudeTool.cpp
    src/EditTool.cpp
    src/DropRouter.cpp
    src/Settings.cpp
    src/Toasts.cpp
    src/mcp/McpProtocol.cpp
    src/mcp/McpServer.cpp
    src/mcp/McpImage.cpp
    src/mcp/McpScript.cpp
    src/mcp/McpViews.cpp
    src/mcp/McpElements.cpp
    src/mcp/McpSculpt.cpp
    src/mcp/McpTools.cpp)

target_include_directories(ForgeEditor PRIVATE src vendor ${tinygltf_SOURCE_DIR}) # src: mcp/ includes editor roots; tinygltf dir: bundled json.hpp
target_link_libraries(ForgeEditor PRIVATE forge imgui imguizmo httplib::httplib lua comdlg32)
# cpp-httplib requires Windows 10 APIs; MinGW-w64 headers default _WIN32_WINNT lower.
target_compile_definitions(ForgeEditor PRIVATE _WIN32_WINNT=0x0A00)
