#include "EditorCamera.h"

#include <imgui.h>

#include <algorithm>

namespace forge {

EditorCamera::EditorCamera(float fovDegrees, float nearClip, float farClip)
    : m_FOV(fovDegrees), m_Near(nearClip), m_Far(farClip)
{
    m_Projection = glm::perspective(glm::radians(m_FOV), m_Aspect, m_Near, m_Far);
    RecalculateView();
}

void EditorCamera::SetViewportSize(float width, float height)
{
    if (width <= 0.0f || height <= 0.0f)
        return;
    m_Aspect = width / height;
    m_Projection = glm::perspective(glm::radians(m_FOV), m_Aspect, m_Near, m_Far);
}

vec3 EditorCamera::Forward() const
{
    return quat(vec3(-m_Pitch, -m_Yaw, 0.0f)) * vec3(0.0f, 0.0f, -1.0f);
}
vec3 EditorCamera::Right() const
{
    return quat(vec3(-m_Pitch, -m_Yaw, 0.0f)) * vec3(1.0f, 0.0f, 0.0f);
}
vec3 EditorCamera::Up() const
{
    return quat(vec3(-m_Pitch, -m_Yaw, 0.0f)) * vec3(0.0f, 1.0f, 0.0f);
}

void EditorCamera::OnUpdate(bool viewportHovered)
{
    ImGuiIO& io = ImGui::GetIO();

    // Keep responding mid-drag even if the cursor leaves the viewport.
    bool dragging = ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
                    ImGui::IsMouseDragging(ImGuiMouseButton_Middle);
    if (!viewportHovered && !dragging)
        return;

    vec2 delta{io.MouseDelta.x, io.MouseDelta.y};

    // Plain LMB never touches the camera — it belongs to selection/gizmos.
    bool orbit = io.KeyAlt && !io.KeyShift && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    bool pan = ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
               (io.KeyAlt && io.KeyShift && ImGui::IsMouseDown(ImGuiMouseButton_Left));

    if (orbit) {
        m_Yaw += delta.x * 0.006f;
        m_Pitch += delta.y * 0.006f;
        m_Pitch = std::clamp(m_Pitch, -1.55f, 1.55f);
    } else if (pan) {
        float speed = m_Distance * 0.0015f;
        m_FocalPoint += -Right() * delta.x * speed + Up() * delta.y * speed;
    }

    if (viewportHovered && io.MouseWheel != 0.0f)
        Zoom(1.0f - io.MouseWheel * 0.1f);

    RecalculateView();
}

void EditorCamera::Focus(const vec3& point, float radius)
{
    m_FocalPoint = point;
    m_Distance = std::clamp(radius * 2.5f, 1.0f, 200.0f);
    RecalculateView();
}

void EditorCamera::Zoom(float factor)
{
    m_Distance = std::clamp(m_Distance * factor, 0.5f, 200.0f);
    RecalculateView();
}

void EditorCamera::SetOrbit(float pitch, float yaw)
{
    m_Pitch = std::clamp(pitch, -1.55f, 1.55f);
    m_Yaw = yaw;
    RecalculateView();
}

void EditorCamera::SetFOV(float degrees)
{
    m_FOV = std::clamp(degrees, 10.0f, 120.0f);
    m_Projection = glm::perspective(glm::radians(m_FOV), m_Aspect, m_Near, m_Far);
}

void EditorCamera::ApplyBookmark(const Bookmark& b)
{
    m_FocalPoint = b.focalPoint;
    m_Distance = b.distance;
    m_Pitch = b.pitch;
    m_Yaw = b.yaw;
    RecalculateView();
}

void EditorCamera::RecalculateView()
{
    m_Position = m_FocalPoint - Forward() * m_Distance;
    m_View = glm::lookAt(m_Position, m_FocalPoint, vec3(0.0f, 1.0f, 0.0f));
}

} // namespace forge
