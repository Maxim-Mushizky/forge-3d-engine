#pragma once

#include <forge/core/Math.h>

namespace forge {

// Unity-style editor camera: Alt+LMB = orbit, MMB (or Alt+Shift+LMB) = pan,
// wheel = zoom. Plain LMB is reserved for selection and gizmos.
// Reads input from ImGui IO — must be updated inside the ImGui frame.
// User-tunable input response (#13): plain data so the settings window can
// write it wholesale; consumed by OnUpdate each frame.
struct CameraTuning {
    float orbitSensitivity = 0.006f; // radians per pixel of drag
    float panSpeed = 0.0015f;        // world units per pixel per unit distance
    float zoomStep = 0.1f;           // distance fraction per wheel notch
    bool invertOrbitY = false;
};

class EditorCamera {
public:
    EditorCamera(float fovDegrees, float nearClip, float farClip);

    // viewportHovered: only react to the mouse when the cursor is over the viewport panel.
    void OnUpdate(bool viewportHovered);
    void SetViewportSize(float width, float height);

    void Focus(const vec3& point, float radius); // F key: frame an object
    void Zoom(float factor);                     // <1 zooms in, >1 zooms out

    void SetOrbit(float pitch, float yaw); // view presets (front/right/top)
    void SetFOV(float degrees);
    float FOV() const { return m_FOV; }

    // Ortho extents derive from distance * FOV so toggling keeps the framing.
    void SetOrthographic(bool ortho);
    bool IsOrthographic() const { return m_Orthographic; }

    struct Bookmark {
        vec3 focalPoint;
        float distance, pitch, yaw;
    };
    Bookmark GetBookmark() const { return {m_FocalPoint, m_Distance, m_Pitch, m_Yaw}; }
    void ApplyBookmark(const Bookmark& b);

    CameraTuning& Tuning() { return m_Tuning; }
    const CameraTuning& Tuning() const { return m_Tuning; }

    const mat4& View() const { return m_View; }
    const mat4& Projection() const { return m_Projection; }
    mat4 ViewProjection() const { return m_Projection * m_View; }
    vec3 Position() const { return m_Position; }
    const vec3& FocalPoint() const { return m_FocalPoint; }

private:
    void RecalculateView();
    void RecalculateProjection();
    vec3 Forward() const;
    vec3 Right() const;
    vec3 Up() const;

    float m_FOV, m_Near, m_Far;
    float m_Aspect = 16.0f / 9.0f;
    bool m_Orthographic = false;
    CameraTuning m_Tuning;

    vec3 m_FocalPoint{0.0f, 0.5f, 0.0f};
    float m_Distance = 8.0f;
    float m_Pitch = 0.45f;
    float m_Yaw = 0.65f;

    vec3 m_Position{0.0f};
    mat4 m_View{1.0f};
    mat4 m_Projection{1.0f};
};

} // namespace forge
