#include "Window.h"

#include "forge/core/Log.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

namespace forge {

static int s_WindowCount = 0;

Window::Window(uint32_t width, uint32_t height, const std::string& title)
    : m_Width(width), m_Height(height)
{
    if (s_WindowCount == 0) {
        glfwSetErrorCallback([](int code, const char* desc) {
            FORGE_ERROR("GLFW error %d: %s", code, desc);
        });
        FORGE_ASSERT(glfwInit() == GLFW_TRUE, "glfwInit failed");
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    m_Handle = glfwCreateWindow((int)width, (int)height, title.c_str(), nullptr, nullptr);
    FORGE_ASSERT(m_Handle != nullptr, "glfwCreateWindow failed");
    ++s_WindowCount;

    glfwMakeContextCurrent(m_Handle);
    SetVSync(true);

    glewExperimental = GL_TRUE; // required on core profiles
    GLenum status = glewInit();
    FORGE_ASSERT(status == GLEW_OK, "glewInit failed: %s", (const char*)glewGetErrorString(status));

    FORGE_INFO("OpenGL %s | %s | %s",
               (const char*)glGetString(GL_VERSION),
               (const char*)glGetString(GL_RENDERER),
               (const char*)glGetString(GL_VENDOR));

    glfwSetWindowUserPointer(m_Handle, this);
    glfwSetFramebufferSizeCallback(m_Handle, [](GLFWwindow* win, int w, int h) {
        auto* self = (Window*)glfwGetWindowUserPointer(win);
        self->m_Width = (uint32_t)w;
        self->m_Height = (uint32_t)h;
        glViewport(0, 0, w, h);
    });
}

Window::~Window()
{
    glfwDestroyWindow(m_Handle);
    if (--s_WindowCount == 0)
        glfwTerminate();
}

bool Window::ShouldClose() const { return glfwWindowShouldClose(m_Handle); }
void Window::SetShouldClose(bool value) { glfwSetWindowShouldClose(m_Handle, value ? GLFW_TRUE : GLFW_FALSE); }
void Window::PollEvents() { glfwPollEvents(); }
void Window::SwapBuffers() { glfwSwapBuffers(m_Handle); }
void Window::SetVSync(bool enabled) { glfwSwapInterval(enabled ? 1 : 0); }
void Window::SetTitle(const std::string& title) { glfwSetWindowTitle(m_Handle, title.c_str()); }

} // namespace forge
