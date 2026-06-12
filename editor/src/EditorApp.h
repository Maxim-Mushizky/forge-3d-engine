#pragma once

#include "CommandStack.h"
#include "EditorCamera.h"
#include "ExtrudeTool.h"
#include "SculptTool.h"

#include <forge/geometry/MeshBoolean.h>
#include <forge/platform/Window.h>
#include <forge/scene/BoxSelect.h>
#include <forge/raytrace/PathTracer.h>
#include <forge/renderer/Framebuffer.h>
#include <forge/renderer/PostProcess.h>
#include <forge/renderer/Renderer.h>
#include <forge/scene/Scene.h>

#include <memory>
#include <string>

struct ImFont;
struct ForgeGifWriter; // wraps gif-h's GifWriter; gif.h is included only in EditorApp.cpp

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
    void MirrorSelected();   // bake X-mirror into the selected mesh (undoable)
    void SubdivideSelected(bool keepShape);
    void RemeshSelected();
    void BooleanSelected(BooleanOp op); // first selected (op) second selected
    void ExportStlDialog();  // save dialog + export selection (or whole scene)

    // Turntable GIF: amortized over UI frames (never blocks the loop).
    void StartTurntableDialog();
    bool StartTurntable(const std::string& path, int frames, int sppTarget);
    void UpdateTurntable(); // one Dispatch slice per UI frame; writes a GIF frame when converged
    void FinishTurntable();
    void DrawTurntableModal();

    // selection (multi-select: last = primary, drives gizmo/inspector)
    void SelectOnly(UUID id);
    void ToggleSelection(UUID id);
    bool IsSelected(UUID id) const;
    // Marquee: select every entity whose projected bounds overlap rect (viewport
    // UV). Parts of groups resolve to the root, matching click behavior.
    void ApplyBoxSelect(const RectUV& rect, bool additive);

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
    void OpenSceneFile(const std::string& path); // CLI arg / recents / drag-drop
private:
    // --- scene file lifecycle (#1) ---------------------------------------
    enum class FileAction { None, NewScene, OpenScene, Exit };
    void DrawMainMenuBar(); // also owns the unsaved-changes modal
    void DoNewScene();
    bool SaveScene();   // Save As when untitled; true once written
    bool SaveSceneAs();
    void RequestWithUnsavedCheck(FileAction action, const std::string& openPath = "");
    void ExecutePendingAction();
    // Dirty = entity edits (command revision) OR scene-level settings changes
    // (sun/sky/RT/export — hashed). Camera pose is deliberately excluded:
    // orbiting around your model isn't unsaved work.
    bool SceneDirty() const;
    void MarkSaved();
    uint64_t SettingsHash() const;
    void UpdateWindowTitle();
    std::string BuildExtrasJson() const;
    void ApplyExtrasJson(const std::string& extras);
    std::string MeshRecipe(const Mesh* mesh) const;            // "" = not a shared primitive
    std::shared_ptr<Mesh> MeshFromRecipe(const std::string& recipe) const;
    void AddRecentFile(const std::string& path);
    void LoadRecentFiles();
    void SaveRecentFiles() const;
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
    ExtrudeTool m_Extrude;
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
    bool m_Denoise = true;
    float m_DenoiseStrength = 0.7f;
    float m_Aperture = 0.0f;    // 0 = DOF off
    float m_FocusDist = -1.0f;  // <0 = uninitialized; defaults to the orbit distance
    float m_LastAperture = 0.0f, m_LastFocusDist = -1.0f;
    float m_StlScale = 100.0f;  // mm per scene unit
    std::string m_StlStatus;    // last export outcome, shown under the button
    bool m_SubdivKeepShape = false;
    int m_RemeshDetail = 64; // 96+ gets very dense (a cube remeshes to ~150k tris at 96)
    std::string m_BoolStatus; // last boolean error, shown in the Modify section
    char m_TextInput[64] = "Forge";
    float m_TextDepth = 0.25f;

    std::string m_ScenePath;        // empty = untitled
    std::string m_EnvPath;          // HDRI source path (for serialization)
    uint64_t m_SavedRevision = 0;     // CommandStack revision at last save
    uint64_t m_SavedSettingsHash = 0; // SettingsHash() at last save
    std::vector<std::string> m_RecentFiles;
    FileAction m_PendingAction = FileAction::None;
    std::string m_PendingOpenPath;
    bool m_ShowUnsavedModal = false;
    bool m_ForceClose = false;      // user chose to discard on exit
    std::string m_LastTitle;

    struct TurntableJob {
        bool active = false;
        int frame = 0, totalFrames = 48;
        int sppDone = 0, sppTarget = 128;
        float baseYaw = 0.0f, pitch = 0.0f;
        EditorCamera::Bookmark restore{};
        ForgeGifWriter* writer = nullptr;
    };
    TurntableJob m_Turntable;
    float m_RTScale = 0.75f;          // render-resolution fraction (Intel Arc relief)
    double m_RTSettleAt = 0.0;        // geometry edits suspend RT until this time
    bool m_RTUploadPending = false;   // BVH rebuild deferred until the scene settles
    uint64_t m_LastSceneHash = 0;
    mat4 m_LastViewProj{0.0f};
    DirectionalLight m_LastSun;
    int m_LastBounces = 0;

    std::shared_ptr<Mesh> m_CubeMesh, m_SphereMesh, m_PlaneMesh, m_CylinderMesh, m_ConeMesh, m_TorusMesh;
    std::shared_ptr<Mesh> m_SculptSphereMesh, m_TerrainMesh; // high-res, sculpt-ready

    bool m_BoxSelecting = false; // LMB went down on empty space; drag = marquee
    vec2 m_BoxStartUV{0.0f};

    bool m_ViewportHovered = false;
    vec2 m_ViewportPos{0.0f};  // screen-space top-left of the viewport image
    vec2 m_ViewportSize{1280.0f, 720.0f};
    int m_SpawnCounter = 1;
};

} // namespace forge
