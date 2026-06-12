#pragma once

#include "CommandStack.h"
#include "EditorCamera.h"
#include "SculptTool.h"

#include <forge/platform/Window.h>
#include <forge/raytrace/PathTracer.h>
#include <forge/renderer/Framebuffer.h>
#include <forge/renderer/PostProcess.h>
#include <forge/renderer/Renderer.h>
#include <forge/scene/Scene.h>

#include <memory>

struct ImFont;

namespace forge {

enum class GizmoOp { Translate, Rotate, Scale };

class EditorApp {
public:
    EditorApp();
    ~EditorApp();

    void Run();

private:
    void RenderScene();
    void HandleShortcuts();
    void DrawSidebar();
    void DrawHierarchy();
    void DrawInspector();
    void DrawViewport();
    void BuildDockLayoutIfNeeded(unsigned int dockspaceID);

    void SpawnPrimitive(const char* baseName, const std::shared_ptr<Mesh>& mesh, float yOffset);
    void SpawnPointLight();
    void LoadHDRI();

    // selection (multi-select: last = primary, drives gizmo/inspector)
    void SelectOnly(UUID id);
    void ToggleSelection(UUID id);
    bool IsSelected(UUID id) const;

    // hierarchy ops
    std::vector<UUID> SubtreeOf(UUID root) const; // root first
    void DeleteSelected();
    void DuplicateSelected();
    void GroupSelection();
    void UngroupSelected();
    void DrawHierarchyNode(Entity& e);
    void ImportModel();                          // file dialog
public:
    void ImportModel(const std::string& path);   // direct (CLI arg / drag-drop)
    void SetRayTracing(bool enabled) { m_RayTracing = enabled; }
    bool LoadHDRIFile(const std::string& path);
    void ToggleSculptMode();
private:
    void UpdateRayTracer();
    void GatherLights();
    uint64_t SceneHash() const;
    // Build a world-space ray through a point on the viewport image (uv in [0,1]).
    Ray ViewportRay(const vec2& uv) const;

    Window m_Window;
    Renderer m_Renderer;
    Framebuffer m_Framebuffer; // HDR (RGBA16F)
    PostProcess m_Post;
    uint32_t m_DisplayTex = 0; // post-processed LDR shown in the viewport
    EditorCamera m_Camera;
    Scene m_Scene;
    CommandStack m_Commands;

    UUID m_Selected = 0;                // primary selection
    std::vector<UUID> m_Selection;      // full selection set (contains primary)
    ImFont* m_BodyFont = nullptr;
    ImFont* m_HeaderFont = nullptr;
    SculptTool m_Sculpt;
    GizmoOp m_GizmoOp = GizmoOp::Translate;
    bool m_GizmoWasUsing = false;
    Entity m_BeforeEdit; // snapshot taken when a gizmo drag / widget edit begins
    bool m_FirstDockLayout = false;

    DirectionalLight m_Sun;
    float m_SunAzimuth = 40.0f, m_SunElevation = 50.0f;
    std::unique_ptr<Environment> m_Env;
    std::vector<PointLightDraw> m_FrameLights; // gathered each frame

    struct CameraBookmark {
        bool set = false;
        EditorCamera::Bookmark value;
    };
    CameraBookmark m_Bookmarks[4]; // F1..F4 recall, Ctrl+F1..F4 store

    bool m_RayTracing = false;
    PathTracer m_PathTracer;
    int m_Bounces = 4;
    float m_RTScale = 0.75f;          // render-resolution fraction (Intel Arc relief)
    double m_RTSettleAt = 0.0;        // geometry edits suspend RT until this time
    bool m_RTUploadPending = false;   // BVH rebuild deferred until the scene settles
    uint64_t m_LastSceneHash = 0;
    mat4 m_LastViewProj{0.0f};
    DirectionalLight m_LastSun;
    int m_LastBounces = 0;

    std::shared_ptr<Mesh> m_CubeMesh, m_SphereMesh, m_PlaneMesh, m_CylinderMesh, m_ConeMesh, m_TorusMesh;
    std::shared_ptr<Mesh> m_SculptSphereMesh, m_TerrainMesh; // high-res, sculpt-ready

    bool m_ViewportHovered = false;
    vec2 m_ViewportPos{0.0f};  // screen-space top-left of the viewport image
    vec2 m_ViewportSize{1280.0f, 720.0f};
    int m_SpawnCounter = 1;
};

} // namespace forge
