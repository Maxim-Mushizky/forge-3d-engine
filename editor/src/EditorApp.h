#pragma once

#include "CommandStack.h"
#include "EditorCamera.h"
#include "EditTool.h"
#include "ExtrudeTool.h"
#include "SculptTool.h"
#include "Settings.h"
#include "Toasts.h"
#include "mcp/McpServer.h"

#include <forge/geometry/MeshBoolean.h>
#include <forge/platform/Window.h>
#include <forge/scene/BoxSelect.h>
#include <forge/scene/TransformSnap.h>
#include <forge/raytrace/PathTracer.h>
#include <forge/renderer/Framebuffer.h>
#include <forge/renderer/PostProcess.h>
#include <forge/renderer/Renderer.h>
#include <forge/scene/Scene.h>

#include <memory>
#include <string>
#include <unordered_set>

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
    void DrawHistorySection(); // undo-history list inside the sidebar (#23)
    void DrawHelpOverlay();    // shortcut cheat-sheet, toggled with '?' (#10)
    void BuildDockLayoutIfNeeded(unsigned int dockspaceID);

    void SpawnPrimitive(const char* baseName, const std::shared_ptr<Mesh>& mesh, float yOffset);
    void SpawnPointLight();
    void LoadHDRI();
    void MirrorSelected();   // bake X-mirror into the selected mesh (undoable)
    void SubdivideSelected(bool keepShape);
    void DropSelectedToGround(); // rest each selected root on the surface below it
    void RemeshSelected();
    void UnwrapSelected(); // regenerate a non-overlapping UV atlas (undoable, #81)
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
    bool ImportModel(const std::string& path);   // direct (CLI arg / drag-drop); false = load failed
    void SetRayTracing(bool enabled) { m_RayTracing = enabled; }
    bool LoadHDRIFile(const std::string& path);
    void ToggleSculptMode();
    void ToggleEditMode();
    void ArmEditExtrude(); // start an edit-mode extrude drag + set its world slide line
    void OpenSceneFile(const std::string& path); // CLI arg / recents / drag-drop
    void ForceEnableMcp(); // --mcp CLI flag: run the server without persisting the pref
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
    // Drag-drop (#2): the OS drop lands mid-PollEvents, so files are queued with
    // the cursor position and routed once per frame (viewport rect is stable then).
    void ProcessPendingDrops();
    void DropImageOnViewport(const std::string& path, const vec2& cursorPx);
    // --- preferences (#13) ------------------------------------------------
    void LoadSettings();      // read forge_settings.json, apply, seed live values
    void SaveSettings();      // sync live values back and rewrite the file
    void ApplySettings();     // push camera tuning / font scale / tooltips
    void QueueSettingsSave(); // debounce: rewrite shortly after the last edit
    void DrawSettingsWindow();
    // --- MCP server (#75/#76/#77) — tool bodies live in mcp/McpTools.cpp ----
    void RegisterMcpTools();  // fills m_McpProtocol
    void UpdateMcpServer();   // start/stop/restart to match settings + CLI flag
    void UpdateMcpRender();   // one path-tracer slice per UI frame (like turntable)
    ToolResult ToolGetScene(const nlohmann::json& args);
    ToolResult ToolGetEntity(const nlohmann::json& args);
    ToolResult ToolGetMeshStats(const nlohmann::json& args);
    ToolResult ToolRaycast(const nlohmann::json& args);
    ToolResult ToolCheckOverlap(const nlohmann::json& args);  // #94: AABB interpenetration
    ToolResult ToolQuerySpatial(const nlohmann::json& args);  // #94: radius/height/ground queries
    ToolResult ToolPlaceRelative(const nlohmann::json& args); // #95: on/above/against/facing/around
    ToolResult ToolSnapToSurface(const nlohmann::json& args); // #95: rest on surface (down or ray)
    ToolResult ToolArrangeEntities(const nlohmann::json& args); // #95: align / distribute
    // #91: script-only bindings (forge.* Lua surface, no top-level MCP tools)
    ToolResult ToolCameraOp(const nlohmann::json& args);        // pose / FOV / bookmarks
    ToolResult ToolSelectOp(const nlohmann::json& args);        // select / toggle / box
    ToolResult ToolSceneStructure(const nlohmann::json& args);  // group / ungroup / drop
    ToolResult ToolSnapSettings(const nlohmann::json& args);    // gizmo snap prefs
    ToolResult ToolMeshElements(const nlohmann::json& args);    // face/edge id queries
    ToolResult ToolEditElements(const nlohmann::json& args);    // extrude/subdivide/shade
    ToolResult ToolSculpt(const nlohmann::json& args);          // #105: sculpt / move_verts
    ToolResult ToolExportStl(const nlohmann::json& args);
    // Combined world AABB of an entity and its descendants; optionally
    // collects the subtree's ids (for excluding it from collision queries).
    AABB SubtreeWorldBounds(UUID root, std::unordered_set<UUID>* members);
    ToolResult ToolManageEntity(const nlohmann::json& args);
    ToolResult ToolManageMaterial(const nlohmann::json& args);
    ToolResult ToolManageLight(const nlohmann::json& args);
    ToolResult ToolEditMesh(const nlohmann::json& args);
    ToolResult ToolManageScene(const nlohmann::json& args);
    ToolResult ToolExecuteScript(const nlohmann::json& args); // #78: Lua, one undo entry
    ToolResult ToolRenderViews(const nlohmann::json& args);   // #93: multi-view raster diagnostics

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
    EditTool m_Edit;
    ExtrudeTool m_Extrude;
    ToastManager m_Toasts;     // transient success/error notifications (#6)
    bool m_ShowHelp = false;   // shortcut cheat-sheet overlay (#10)
    Settings m_Settings;           // persistent preferences (#13)
    bool m_ShowSettings = false;   // Preferences window visibility
    double m_SettingsSaveAt = 0.0; // 0 = no save pending (debounced write)
    GizmoOp m_GizmoOp = GizmoOp::Translate;
    bool m_GizmoWasUsing = false;
    // Transform snapping (#5): quantize gizmo drags and inspector fields to
    // fixed increments. The toggle persists; holding Ctrl flips it for a single
    // gesture (industry standard). Defaults seeded from Settings (#13); live
    // values sync back into the file on save.
    bool m_SnapEnabled = false;
    float m_SnapTranslate = 0.25f; // world units per grid step
    float m_SnapRotateDeg = 15.0f; // degrees per angle step
    float m_SnapScale = 0.1f;      // scale-factor step
    // Edit-mode element gizmo: the matrix ImGuizmo manipulates, the frame it
    // started at (for the world delta), and whether a drag is in progress.
    mat4 m_EditGizmo{1.0f};
    mat4 m_EditGizmoStart{1.0f};
    bool m_EditGizmoUsing = false;
    // Edit-mode face extrude (#63): the world-space line the cap slides along
    // (anchor + unit direction) and the world units per object-space unit, set
    // when BeginExtrude arms the drag (see ExtrudeTool for the same mapping).
    vec3 m_EditExtrudeLineP{0.0f};
    vec3 m_EditExtrudeLineD{0.0f, 1.0f, 0.0f};
    float m_EditExtrudeWorldPerLocal = 1.0f;
    float m_EditSmoothStrength = 0.5f; // edit-mode Laplacian smooth strength (#65)
    Entity m_BeforeEdit; // snapshot taken when a gizmo drag / widget edit begins
    int m_InspectorSlot = 0; // material slot shown in the inspector (#80), clamped per entity
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
    int m_AdaptiveSpp = 4; // samples per frame while idle; grows/shrinks with frame time
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
    // Inspector UV readout cache (#81): the coverage sum is O(tris), recompute
    // only when the selected mesh (identity or version) changes.
    std::weak_ptr<Mesh> m_UvStatsMesh;
    uint64_t m_UvStatsVersion = 0;
    float m_UvStatsCoverage = 0.0f; // summed UV-space triangle area
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

    struct PendingDrop {
        std::vector<std::string> paths;
        vec2 cursorPx; // window client coords at drop time
    };
    std::vector<PendingDrop> m_PendingDrops;

    // MCP (#75): protocol declared before the server so the server (which
    // holds a reference to it) is destroyed first.
    McpProtocol m_McpProtocol;
    std::unique_ptr<McpServer> m_McpServer; // null = off
    bool m_McpCliForced = false;            // --mcp: enabled for this run only

    // render_image (#76): amortized like the turntable — ~8 spp per UI frame,
    // the MCP response resolves when sppTarget is reached.
    struct McpRenderJob {
        bool active = false;
        int sppTarget = 256;
        int bounces = 4;        // per-render override (#92); editor m_Bounces untouched
        mat4 viewProj{1.0f};    // frozen at start: requested aspect, current camera
        vec3 camPos{0.0f};      // frozen with viewProj — a live camera position
                                // against a frozen matrix corrupts accumulation
        ToolResponder respond;  // held until the render converges
    };
    McpRenderJob m_McpRender;
};

} // namespace forge
