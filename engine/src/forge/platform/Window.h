#pragma once

#include <cstdint>
#include <string>

struct GLFWwindow;

namespace forge {

// Owns the OS window and the OpenGL 4.6 core context.
// The rest of the engine never sees GLFW types except the raw handle
// needed by the ImGui backend.
class Window {
public:
    Window(uint32_t width, uint32_t height, const std::string& title);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool ShouldClose() const;
    void SetShouldClose(bool value); // intercept close for unsaved-changes prompts
    void PollEvents();
    void SwapBuffers();
    void SetVSync(bool enabled);
    void SetTitle(const std::string& title);

    uint32_t Width() const { return m_Width; }
    uint32_t Height() const { return m_Height; }
    GLFWwindow* NativeHandle() const { return m_Handle; }

private:
    GLFWwindow* m_Handle = nullptr;
    uint32_t m_Width;
    uint32_t m_Height;
};

} // namespace forge
