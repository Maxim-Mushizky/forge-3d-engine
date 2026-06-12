#pragma once

#include "CommandStack.h"

#include <forge/core/Math.h>
#include <forge/geometry/MeshEdit.h>
#include <forge/scene/Scene.h>

#include <memory>
#include <vector>

namespace forge {

class EditorCamera;

// Brush-based fixed-topology sculpting (Blender/Nomad core brush set).
// LMB sculpt, Ctrl inverts, Shift = smooth, X-mirror on by default.
// One stroke (press -> release) = one undo step (sparse vertex diff).
class SculptTool {
public:
    enum class Brush { Draw, Smooth, Grab, Inflate };

    bool Active() const { return m_Active; }
    UUID Target() const { return m_Target; }

    void Enter(Scene& scene, UUID entity); // copy-on-write clone + topology build
    void Exit();

    // Per-frame from the viewport panel. Returns a finished stroke command or null.
    std::unique_ptr<Command> OnViewportFrame(Scene& scene, const EditorCamera& camera,
                                             const vec2& viewportPos, const vec2& viewportSize,
                                             bool viewportHovered);

    void DrawSettingsUI();

private:
    void BeginStroke(Scene& scene, const RaycastHit& hit, const EditorCamera& camera, bool smoothOverride);
    std::unique_ptr<Command> EndStroke(Scene& scene);
    void ApplyDab(Entity& e, const vec3& localPos, const vec3& localDir, float localRadius,
                  float amount, bool smoothMode);
    void CaptureGrab(Entity& e, const vec3& localPos, float localRadius, bool mirrored);

    bool m_Active = false;
    UUID m_Target = 0;
    MeshTopology m_Topology;

    Brush m_Brush = Brush::Draw;
    float m_Radius = 0.4f;
    float m_Strength = 0.5f;
    bool m_MirrorX = true;

    // stroke state
    bool m_Stroking = false;
    bool m_StrokeIsSmooth = false;
    std::vector<Vertex> m_StrokeBefore;
    bool m_HaveLastDab = false;
    vec3 m_LastDabWorld{0.0f};

    // grab state
    struct GrabVert {
        uint32_t index;
        float weight;
        vec3 startPos;
        bool mirrored;
    };
    std::vector<GrabVert> m_GrabVerts;
    vec3 m_GrabPlanePoint{0.0f};
    vec3 m_GrabPlaneNormal{0.0f, 0.0f, 1.0f};
};

} // namespace forge
