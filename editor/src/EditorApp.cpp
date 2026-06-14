#include "EditorApp.h"
#include "FileDialog.h"
#include "Theme.h"

#include <forge/assets/AssetManager.h>
#include <forge/assets/MeshFactory.h>
#include <forge/assets/SceneSerializer.h>
#include <forge/assets/StlExporter.h>
#include <forge/core/Log.h>
#include <forge/geometry/MeshEdit.h>
#include <forge/geometry/MeshRemesh.h>
#include <forge/scene/DropToGround.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <ImGuizmo.h>

#include <gif.h>   // single TU only: gif-h's functions are not inline
#include <json.hpp> // nlohmann, bundled with tinygltf (extras blob for scene files)

#include <fstream>

// gif-h's GifWriter is an anonymous-struct typedef, so it can't be forward
// declared — this wrapper gives the header a nameable opaque type.
struct ForgeGifWriter {
    GifWriter w{};
};

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace forge {

EditorApp::EditorApp()
    : m_Window(1600, 900, "Forge Editor"),
      m_Framebuffer(1280, 720, /*hdr=*/true),
      m_Camera(45.0f, 0.1f, 500.0f)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "forge_ui.ini"; // own layout file (panel names changed across versions)

    ui::ApplyTheme();

    // Real fonts: the default 13px pixel font is the single biggest "demo app" signal.
    float dpiScale = 1.0f, dpiY = 1.0f;
    glfwGetWindowContentScale(m_Window.NativeHandle(), &dpiScale, &dpiY);
    ImFontConfig cfg;
    m_BodyFont = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 17.0f * dpiScale, &cfg);
    m_HeaderFont = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisb.ttf", 20.0f * dpiScale, &cfg);
    if (!m_BodyFont) // exotic Windows install: fall back to the built-in font
        m_HeaderFont = m_BodyFont = io.Fonts->AddFontDefault();

    m_FirstDockLayout = !std::filesystem::exists("forge_ui.ini");

    ImGui_ImplGlfw_InitForOpenGL(m_Window.NativeHandle(), true);
    ImGui_ImplOpenGL3_Init("#version 460");

    m_Renderer.Init();
    m_PathTracer.Init();
    m_Post.Init();

    m_CubeMesh = MeshFactory::Cube();
    m_SphereMesh = MeshFactory::Sphere();
    m_PlaneMesh = MeshFactory::Plane(2.0f);
    m_CylinderMesh = MeshFactory::Cylinder();
    m_ConeMesh = MeshFactory::Cone();
    m_TorusMesh = MeshFactory::Torus();
    m_SculptSphereMesh = MeshFactory::Sphere(64, 96);  // ~6k verts, sculpt-ready
    m_TerrainMesh = MeshFactory::Plane(8.0f, 96);      // ~9.4k verts, terrain sculpting

    // Starter object so the first launch isn't empty.
    Entity& cube = m_Scene.CreateEntity("Cube");
    cube.mesh = m_CubeMesh;
    cube.transform.translation = {0.0f, 0.5f, 0.0f};
    cube.material.albedo = {0.85f, 0.35f, 0.25f};

    LoadRecentFiles();
    MarkSaved(); // the starter scene isn't "unsaved work"
}

EditorApp::~EditorApp()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void EditorApp::Run()
{
    while (true) {
        m_Window.PollEvents();

        // Close intercept: the X button must respect unsaved work.
        if (m_Window.ShouldClose()) {
            if (!SceneDirty() || m_ForceClose)
                break;
            m_Window.SetShouldClose(false);
            m_PendingAction = FileAction::Exit;
            m_ShowUnsavedModal = true;
        }

        UpdateWindowTitle();

        float az = glm::radians(m_SunAzimuth), el = glm::radians(m_SunElevation);
        m_Sun.direction = -vec3(std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az));

        // UI first: camera input is processed inside the viewport panel so
        // ImGui's wheel/mouse state is fresh, then the scene renders with
        // this frame's camera before the draw data (which samples the
        // viewport texture) is submitted.
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();

        DrawMainMenuBar(); // before the dockspace so it claims the top work area
        unsigned int dockspaceID = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        BuildDockLayoutIfNeeded(dockspaceID);

        HandleShortcuts();
        DrawSidebar();
        DrawHierarchy();
        DrawInspector();
        DrawViewport(); // updates the camera
        ImGui::Render();

        // Raster while sculpting: mid-stroke mesh edits would otherwise trigger a
        // BVH rebuild every frame. Same for geometry edits in RT mode: raster
        // preview until the scene settles (m_RTUploadPending).
        bool rtActive = (m_RayTracing && !m_Sculpt.Active()) || m_Turntable.active;
        if (m_Turntable.active)
            UpdateTurntable(); // drives the path tracer directly, bypasses settle logic
        else if (rtActive)
            UpdateRayTracer();
        if (!rtActive || m_RTUploadPending)
            RenderScene();

        glViewport(0, 0, (GLsizei)m_Window.Width(), (GLsizei)m_Window.Height());
        glClearColor(0.086f, 0.090f, 0.102f, 1.0f); // matches the theme's TitleBg
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        m_Window.SwapBuffers();
    }
}

void EditorApp::GatherLights()
{
    m_FrameLights.clear();
    for (const Entity& e : m_Scene.Entities())
        if (e.light.enabled)
            m_FrameLights.push_back({vec3(m_Scene.WorldTransform(e.id)[3]),
                                     e.light.color * e.light.intensity, e.light.range});
}

void EditorApp::RenderScene()
{
    m_Renderer.SetEnvironment(m_Env.get());
    m_Renderer.BeginScene(m_Camera.ViewProjection(), m_Camera.Position(), m_Sun);
    for (const Entity& e : m_Scene.Entities()) {
        mat4 world = m_Scene.WorldTransform(e.id);
        if (e.light.enabled)
            m_Renderer.SubmitLight(vec3(world[3]), e.light.color, e.light.intensity, e.light.range);
        if (!e.mesh)
            continue;
        // Light gizmo meshes don't cast shadows (a light casting its own shadow looks broken).
        m_Renderer.Submit(*e.mesh, world, e.material, !e.light.enabled);
    }
    // Every selected entity gets an outline (primary brighter); selecting a
    // group highlights its whole subtree, since the group node has no mesh.
    if (!m_Sculpt.Active()) {
        for (UUID id : m_Selection) {
            vec3 color = id == m_Selected ? vec3(1.0f, 0.6f, 0.1f) : vec3(0.95f, 0.78f, 0.4f);
            for (UUID node : SubtreeOf(id))
                if (Entity* e = m_Scene.Find(node); e && e->mesh)
                    m_Renderer.AddOutline(*e->mesh, m_Scene.WorldTransform(node), color);
        }
    }
    m_Renderer.EndScene(m_Framebuffer); // shadow pass + main pass
    m_Framebuffer.Unbind();

    m_DisplayTex = m_Post.Process(m_Framebuffer.ColorAttachment(), m_Framebuffer.Width(), m_Framebuffer.Height());
}

uint64_t EditorApp::SceneHash() const
{
    // FNV-1a over everything the path tracer consumes. Change -> re-upload + restart accumulation.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* data, size_t bytes) {
        const uint8_t* p = (const uint8_t*)data;
        for (size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    for (const Entity& e : m_Scene.Entities()) {
        mix(&e.id, sizeof(e.id));
        mix(&e.parent, sizeof(e.parent)); // group moves must refresh the path tracer
        mix(&e.transform, sizeof(e.transform));
        if (!e.mesh && !e.light.enabled)
            continue;
        mix(&e.material.albedo, sizeof(e.material.albedo));
        mix(&e.material.metallic, sizeof(e.material.metallic));
        mix(&e.material.roughness, sizeof(e.material.roughness));
        mix(&e.material.emissive, sizeof(e.material.emissive));
        mix(&e.material.emissiveStrength, sizeof(e.material.emissiveStrength));
        mix(&e.material.transmission, sizeof(e.material.transmission));
        mix(&e.material.ior, sizeof(e.material.ior));
        mix(&e.light, sizeof(e.light));
        const Mesh* mesh = e.mesh.get();
        mix(&mesh, sizeof(mesh));
        if (e.mesh) {
            uint64_t v = e.mesh->Version(); // sculpt edits change content, not the pointer
            mix(&v, sizeof(v));
        }
    }
    return h;
}

void EditorApp::UpdateRayTracer()
{
    m_PathTracer.Resize((uint32_t)(m_ViewportSize.x * m_RTScale), (uint32_t)(m_ViewportSize.y * m_RTScale));

    uint64_t hash = SceneHash();
    hash ^= m_PathTracer.GroundPlane() ? 0x9E3779B97F4A7C15ull : 0; // floor toggle re-uploads
    if (hash != m_LastSceneHash) {
        // Geometry changed: rebuilding the BVH every frame mid-drag tanks the
        // frame rate, so defer until the scene has been stable for a moment.
        // The viewport falls back to raster while pending.
        m_RTUploadPending = true;
        m_RTSettleAt = glfwGetTime() + 0.30;
        m_LastSceneHash = hash;
    }
    if (m_RTUploadPending) {
        if (glfwGetTime() < m_RTSettleAt)
            return; // still settling — raster preview this frame
        m_PathTracer.Upload(m_Scene);
        m_PathTracer.ResetAccumulation();
        m_RTUploadPending = false;
    }

    GatherLights();

    // Environment changes also restart accumulation (cheap state compare via local hash).
    float envState[3] = {m_Env && m_Env->Valid() ? (float)m_Env->Source() : 0.0f,
                         m_Env ? m_Env->intensity : 0.0f,
                         m_Env ? m_Env->rotationDegrees : 0.0f};
    static float lastEnvState[3] = {-1, -1, -1};
    bool envChanged = std::memcmp(envState, lastEnvState, sizeof(envState)) != 0;
    std::memcpy(lastEnvState, envState, sizeof(envState));

    // Thin-lens DOF. Ortho has parallel rays — no lens point, force pinhole.
    if (m_FocusDist <= 0.0f)
        m_FocusDist = glm::length(m_Camera.FocalPoint() - m_Camera.Position());
    float aperture = m_Camera.IsOrthographic() ? 0.0f : m_Aperture;
    const mat4& view = m_Camera.View();
    m_PathTracer.SetLens(aperture, m_FocusDist, vec3(view[0][0], view[1][0], view[2][0]),
                         vec3(view[0][1], view[1][1], view[2][1]));

    mat4 viewProj = m_Camera.ViewProjection();
    bool reset = viewProj != m_LastViewProj || std::memcmp(&m_Sun, &m_LastSun, sizeof(m_Sun)) != 0 ||
                 m_Bounces != m_LastBounces || envChanged || aperture != m_LastAperture ||
                 m_FocusDist != m_LastFocusDist;
    if (reset)
        m_PathTracer.ResetAccumulation();
    m_LastViewProj = viewProj;
    m_LastSun = m_Sun;
    m_LastBounces = m_Bounces;
    m_LastAperture = aperture;
    m_LastFocusDist = m_FocusDist;

    // Frame-budget adaptive sampling: 1 spp while interacting (latency); while
    // idle, grow samples per frame as long as the frame time stays under budget
    // so a fast GPU converges at its own throughput instead of the UI frame
    // rate (the shader loops u_SamplesPerPass sub-samples in one dispatch).
    // Under vsync the swap blocks when the GPU falls behind, so the UI frame
    // time is an honest feedback signal; gradual doubling keeps any single
    // dispatch far from the Windows TDR limit.
    int spp = 1;
    if (reset) {
        m_AdaptiveSpp = 4; // restart gently after interaction
    } else if (m_PathTracer.SampleCount() >= 8192) {
        return; // converged long ago — stop burning the GPU
    } else {
        float dt = ImGui::GetIO().DeltaTime;
        if (dt < 1.0f / 45.0f)
            m_AdaptiveSpp = std::min(m_AdaptiveSpp * 2, 128);
        else if (dt > 1.0f / 20.0f)
            m_AdaptiveSpp = std::max(m_AdaptiveSpp / 2, 1);
        spp = m_AdaptiveSpp;
    }
    m_PathTracer.SetDenoise(m_Denoise, m_DenoiseStrength);
    m_PathTracer.Dispatch(viewProj, m_Camera.Position(), m_Sun, m_Bounces, m_FrameLights, m_Env.get(), spp);
}

void EditorApp::BuildDockLayoutIfNeeded(unsigned int dockspaceID)
{
    if (!m_FirstDockLayout)
        return;
    m_FirstDockLayout = false;

    ImGui::DockBuilderRemoveNode(dockspaceID);
    ImGui::DockBuilderAddNode(dockspaceID, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceID, ImGui::GetMainViewport()->Size);
    ImGuiID center = dockspaceID;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.6f, nullptr, &right);
    ImGui::DockBuilderDockWindow("Tools", left);
    ImGui::DockBuilderDockWindow("Scene", right);
    ImGui::DockBuilderDockWindow("Inspector", rightBottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockspaceID);
}

void EditorApp::HandleShortcuts()
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput)
        return;

    if (ImGui::IsKeyPressed(ImGuiKey_W)) m_GizmoOp = GizmoOp::Translate;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) m_GizmoOp = GizmoOp::Rotate;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) m_GizmoOp = GizmoOp::Scale;

    // F = frame selected object (standard "focus" shortcut).
    if (ImGui::IsKeyPressed(ImGuiKey_F)) {
        if (Entity* e = m_Scene.Find(m_Selected)) {
            float radius = 1.0f;
            if (e->mesh) {
                vec3 extent = (e->mesh->Bounds().max - e->mesh->Bounds().min) * e->transform.scale;
                radius = std::max(glm::length(extent) * 0.5f, 0.25f);
            }
            m_Camera.Focus(e->transform.translation, radius);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
        m_Camera.Zoom(0.8f);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
        m_Camera.Zoom(1.25f);

    // View presets (Blender-style): 1 front, 3 right, 7 top; Ctrl = opposite side.
    float side = io.KeyCtrl ? glm::pi<float>() : 0.0f;
    if (ImGui::IsKeyPressed(ImGuiKey_1) || ImGui::IsKeyPressed(ImGuiKey_Keypad1))
        m_Camera.SetOrbit(0.0f, 0.0f + side);
    if (ImGui::IsKeyPressed(ImGuiKey_3) || ImGui::IsKeyPressed(ImGuiKey_Keypad3))
        m_Camera.SetOrbit(0.0f, glm::half_pi<float>() + side);
    if (ImGui::IsKeyPressed(ImGuiKey_7) || ImGui::IsKeyPressed(ImGuiKey_Keypad7))
        m_Camera.SetOrbit(io.KeyCtrl ? -1.55f : 1.55f, 0.0f);

    // Camera bookmarks: Ctrl+F1..F4 store, F1..F4 recall.
    for (int i = 0; i < 4; ++i) {
        if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_F1 + i))) {
            if (io.KeyCtrl) {
                m_Bookmarks[i] = {true, m_Camera.GetBookmark()};
            } else if (m_Bookmarks[i].set) {
                m_Camera.ApplyBookmark(m_Bookmarks[i].value);
            }
        }
    }

    // File ops (Ctrl+Shift+S checked before Ctrl+S).
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_N))
        RequestWithUnsavedCheck(FileAction::NewScene);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O))
        RequestWithUnsavedCheck(FileAction::OpenScene);
    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S))
        SaveSceneAs();
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S))
        SaveScene();

    if (ImGui::IsKeyPressed(ImGuiKey_Delete))
        DeleteSelected();
    if (ImGui::IsKeyPressed(ImGuiKey_End))
        DropSelectedToGround();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D))
        DuplicateSelected();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
        m_Commands.Undo(m_Scene);
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
        m_Commands.Redo(m_Scene);

    if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_G))
        UngroupSelected();
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G))
        GroupSelection();

    if (ImGui::IsKeyPressed(ImGuiKey_Tab))
        ToggleSculptMode();
    if (m_Sculpt.Active() && ImGui::IsKeyPressed(ImGuiKey_Escape))
        m_Sculpt.Exit();
}

void EditorApp::SelectOnly(UUID id)
{
    m_Selected = id;
    m_Selection.clear();
    if (id != 0)
        m_Selection.push_back(id);
}

void EditorApp::ToggleSelection(UUID id)
{
    auto it = std::find(m_Selection.begin(), m_Selection.end(), id);
    if (it != m_Selection.end()) {
        m_Selection.erase(it);
        m_Selected = m_Selection.empty() ? 0 : m_Selection.back();
    } else {
        m_Selection.push_back(id);
        m_Selected = id;
    }
}

void EditorApp::ApplyBoxSelect(const RectUV& rect, bool additive)
{
    std::vector<UUID> picked;
    mat4 vp = m_Camera.ViewProjection();
    for (const Entity& e : m_Scene.Entities()) {
        if (!e.mesh)
            continue; // group nodes get picked through their children's root
        mat4 world = m_Scene.WorldTransform(e.id);
        const AABB& lb = e.mesh->Bounds();
        AABB wb;
        for (int c = 0; c < 8; ++c) {
            vec3 corner{(c & 1) ? lb.max.x : lb.min.x, (c & 2) ? lb.max.y : lb.min.y,
                        (c & 4) ? lb.max.z : lb.min.z};
            wb.Expand(vec3(world * vec4(corner, 1.0f)));
        }
        auto screenRect = ProjectAABBToScreen(vp, wb);
        if (!screenRect || !RectsOverlap(*screenRect, rect))
            continue;
        UUID pick = m_Scene.RootAncestor(e.id);
        if (std::find(picked.begin(), picked.end(), pick) == picked.end())
            picked.push_back(pick);
    }

    if (!additive)
        m_Selection.clear();
    for (UUID id : picked)
        if (std::find(m_Selection.begin(), m_Selection.end(), id) == m_Selection.end())
            m_Selection.push_back(id);
    m_Selected = m_Selection.empty() ? 0 : m_Selection.back();
}

bool EditorApp::IsSelected(UUID id) const
{
    return std::find(m_Selection.begin(), m_Selection.end(), id) != m_Selection.end();
}

std::vector<UUID> EditorApp::SubtreeOf(UUID root) const
{
    std::vector<UUID> result{root};
    for (size_t i = 0; i < result.size(); ++i)
        for (UUID child : m_Scene.ChildrenOf(result[i]))
            result.push_back(child);
    return result;
}

void EditorApp::SpawnPrimitive(const char* baseName, const std::shared_ptr<Mesh>& mesh, float yOffset)
{
    Entity& e = m_Scene.CreateEntity(std::string(baseName) + " " + std::to_string(m_SpawnCounter++));
    e.mesh = mesh;
    e.transform.translation = m_Camera.FocalPoint();
    e.transform.translation.y = yOffset;
    const vec3 palette[] = {{0.85f, 0.35f, 0.25f}, {0.30f, 0.65f, 0.85f}, {0.45f, 0.80f, 0.40f},
                            {0.90f, 0.75f, 0.30f}, {0.70f, 0.45f, 0.85f}};
    e.material.albedo = palette[m_SpawnCounter % 5];
    SelectOnly(e.id);
    m_Commands.Push(std::make_unique<AddEntityCommand>(e));
}

void EditorApp::SpawnPointLight()
{
    Entity& e = m_Scene.CreateEntity("Point Light " + std::to_string(m_SpawnCounter++));
    e.mesh = m_SphereMesh;
    e.transform.translation = m_Camera.FocalPoint() + vec3(0.0f, 2.0f, 0.0f);
    e.transform.scale = vec3(0.2f);
    e.light.enabled = true;
    // The small sphere doubles as the light's gizmo: glows in raster and is a real emitter in RT.
    e.material.albedo = e.light.color;
    e.material.emissive = e.light.color;
    e.material.emissiveStrength = 4.0f;
    SelectOnly(e.id);
    m_Commands.Push(std::make_unique<AddEntityCommand>(e));
}

void EditorApp::ToggleSculptMode()
{
    if (m_Sculpt.Active()) {
        m_Sculpt.Exit();
        return;
    }
    Entity* e = m_Scene.Find(m_Selected);
    if (e && e->mesh)
        m_Sculpt.Enter(m_Scene, e->id);
}

void EditorApp::LoadHDRI()
{
    std::string path = OpenFileDialog(m_Window.NativeHandle(), "HDR Image\0*.hdr\0All Files\0*.*\0");
    if (!path.empty())
        LoadHDRIFile(path);
}

bool EditorApp::LoadHDRIFile(const std::string& path)
{
    auto env = std::make_unique<Environment>();
    if (!env->Load(path))
        return false;
    m_Env = std::move(env);
    m_EnvPath = path;
    return true;
}

// ---------------------------------------------------------------------------
// Scene files (#1)
// ---------------------------------------------------------------------------

std::string EditorApp::MeshRecipe(const Mesh* mesh) const
{
    if (mesh == m_CubeMesh.get()) return "cube";
    if (mesh == m_SphereMesh.get()) return "sphere";
    if (mesh == m_PlaneMesh.get()) return "plane";
    if (mesh == m_CylinderMesh.get()) return "cylinder";
    if (mesh == m_ConeMesh.get()) return "cone";
    if (mesh == m_TorusMesh.get()) return "torus";
    if (mesh == m_SculptSphereMesh.get()) return "sculpt-sphere";
    if (mesh == m_TerrainMesh.get()) return "terrain";
    return "";
}

std::shared_ptr<Mesh> EditorApp::MeshFromRecipe(const std::string& recipe) const
{
    if (recipe == "cube") return m_CubeMesh;
    if (recipe == "sphere") return m_SphereMesh;
    if (recipe == "plane") return m_PlaneMesh;
    if (recipe == "cylinder") return m_CylinderMesh;
    if (recipe == "cone") return m_ConeMesh;
    if (recipe == "torus") return m_TorusMesh;
    if (recipe == "sculpt-sphere") return m_SculptSphereMesh;
    if (recipe == "terrain") return m_TerrainMesh;
    return nullptr;
}

std::string EditorApp::BuildExtrasJson() const
{
    using nlohmann::json;
    json j;
    j["sun"] = {{"azimuth", m_SunAzimuth},
                {"elevation", m_SunElevation},
                {"intensity", m_Sun.intensity},
                {"color", {m_Sun.color.x, m_Sun.color.y, m_Sun.color.z}}};
    j["sky"] = {{"hdri", m_EnvPath},
                {"intensity", m_Env ? m_Env->intensity : 1.0f},
                {"rotation", m_Env ? m_Env->rotationDegrees : 0.0f}};
    j["rt"] = {{"enabled", m_RayTracing}, {"bounces", m_Bounces},     {"scale", m_RTScale},
               {"denoise", m_Denoise},    {"denoiseStrength", m_DenoiseStrength},
               {"aperture", m_Aperture},  {"focus", m_FocusDist},
               {"ground", m_PathTracer.GroundPlane()}};
    EditorCamera::Bookmark cam = m_Camera.GetBookmark();
    j["camera"] = {{"focal", {cam.focalPoint.x, cam.focalPoint.y, cam.focalPoint.z}},
                   {"distance", cam.distance},
                   {"pitch", cam.pitch},
                   {"yaw", cam.yaw},
                   {"ortho", m_Camera.IsOrthographic()}};
    json bookmarks = json::array();
    for (const CameraBookmark& b : m_Bookmarks) {
        if (!b.set) {
            bookmarks.push_back(nullptr);
            continue;
        }
        bookmarks.push_back({{"focal", {b.value.focalPoint.x, b.value.focalPoint.y, b.value.focalPoint.z}},
                             {"distance", b.value.distance},
                             {"pitch", b.value.pitch},
                             {"yaw", b.value.yaw}});
    }
    j["bookmarks"] = std::move(bookmarks);
    j["spawnCounter"] = m_SpawnCounter;
    j["stlScale"] = m_StlScale;
    return j.dump();
}

// Type-checked json getters: scene files are hand-editable by design, so a
// string where a number belongs must skip the field, never throw.
namespace {
float NumOr(const nlohmann::json& o, const char* key, float fallback)
{
    auto it = o.find(key);
    return it != o.end() && it->is_number() ? it->get<float>() : fallback;
}
int IntOr(const nlohmann::json& o, const char* key, int fallback)
{
    auto it = o.find(key);
    return it != o.end() && it->is_number_integer() ? it->get<int>() : fallback;
}
bool BoolOr(const nlohmann::json& o, const char* key, bool fallback)
{
    auto it = o.find(key);
    return it != o.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
std::string StrOr(const nlohmann::json& o, const char* key, const std::string& fallback)
{
    auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : fallback;
}
vec3 Vec3Or(const nlohmann::json& o, const char* key, const vec3& fallback)
{
    auto it = o.find(key);
    if (it == o.end() || !it->is_array() || it->size() != 3 || !(*it)[0].is_number() ||
        !(*it)[1].is_number() || !(*it)[2].is_number())
        return fallback;
    return {(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
}
} // namespace

void EditorApp::ApplyExtrasJson(const std::string& extras)
{
    using nlohmann::json;
    json j = json::parse(extras, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
        return; // old or hand-edited file without extras: keep current settings

    if (auto s = j.find("sun"); s != j.end() && s->is_object()) {
        m_SunAzimuth = NumOr(*s, "azimuth", m_SunAzimuth);
        m_SunElevation = NumOr(*s, "elevation", m_SunElevation);
        m_Sun.intensity = NumOr(*s, "intensity", m_Sun.intensity);
        m_Sun.color = Vec3Or(*s, "color", m_Sun.color);
    }
    if (auto s = j.find("sky"); s != j.end() && s->is_object()) {
        std::string hdri = StrOr(*s, "hdri", "");
        if (hdri.empty()) {
            m_Env.reset();
            m_EnvPath.clear();
        } else if (LoadHDRIFile(hdri)) {
            m_Env->intensity = NumOr(*s, "intensity", 1.0f);
            m_Env->rotationDegrees = NumOr(*s, "rotation", 0.0f);
        } else {
            // The loaded scene must not inherit whatever sky was up before.
            m_Env.reset();
            m_EnvPath.clear();
            FORGE_WARN("Scene references a missing HDRI: %s", hdri.c_str());
        }
    }
    if (auto s = j.find("rt"); s != j.end() && s->is_object()) {
        m_RayTracing = BoolOr(*s, "enabled", m_RayTracing);
        m_Bounces = IntOr(*s, "bounces", m_Bounces);
        m_RTScale = NumOr(*s, "scale", m_RTScale);
        m_Denoise = BoolOr(*s, "denoise", m_Denoise);
        m_DenoiseStrength = NumOr(*s, "denoiseStrength", m_DenoiseStrength);
        m_Aperture = NumOr(*s, "aperture", m_Aperture);
        m_FocusDist = NumOr(*s, "focus", m_FocusDist);
        m_PathTracer.SetGroundPlane(BoolOr(*s, "ground", m_PathTracer.GroundPlane()));
    }
    if (auto s = j.find("camera"); s != j.end() && s->is_object()) {
        EditorCamera::Bookmark cam;
        cam.focalPoint = Vec3Or(*s, "focal", {0.0f, 0.5f, 0.0f});
        cam.distance = NumOr(*s, "distance", 8.0f);
        cam.pitch = NumOr(*s, "pitch", 0.45f);
        cam.yaw = NumOr(*s, "yaw", 0.65f);
        m_Camera.SetOrthographic(BoolOr(*s, "ortho", false));
        m_Camera.ApplyBookmark(cam);
    }
    if (auto s = j.find("bookmarks"); s != j.end() && s->is_array()) {
        for (size_t i = 0; i < 4 && i < s->size(); ++i) {
            const json& b = (*s)[i];
            m_Bookmarks[i].set = b.is_object();
            if (!m_Bookmarks[i].set)
                continue;
            m_Bookmarks[i].value.focalPoint = Vec3Or(b, "focal", vec3(0.0f));
            m_Bookmarks[i].value.distance = NumOr(b, "distance", 8.0f);
            m_Bookmarks[i].value.pitch = NumOr(b, "pitch", 0.0f);
            m_Bookmarks[i].value.yaw = NumOr(b, "yaw", 0.0f);
        }
    }
    m_SpawnCounter = IntOr(j, "spawnCounter", m_SpawnCounter);
    m_StlScale = NumOr(j, "stlScale", m_StlScale);
}

uint64_t EditorApp::SettingsHash() const
{
    // FNV-1a over the scene-level settings BuildExtrasJson serializes, except
    // camera pose/bookmarks (view changes shouldn't flag unsaved work).
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* data, size_t bytes) {
        const uint8_t* p = (const uint8_t*)data;
        for (size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    mix(&m_SunAzimuth, sizeof(m_SunAzimuth));
    mix(&m_SunElevation, sizeof(m_SunElevation));
    mix(&m_Sun.intensity, sizeof(m_Sun.intensity));
    mix(&m_Sun.color, sizeof(m_Sun.color));
    mix(m_EnvPath.data(), m_EnvPath.size());
    float envIntensity = m_Env ? m_Env->intensity : 0.0f;
    float envRotation = m_Env ? m_Env->rotationDegrees : 0.0f;
    mix(&envIntensity, sizeof(envIntensity));
    mix(&envRotation, sizeof(envRotation));
    mix(&m_RayTracing, sizeof(m_RayTracing));
    mix(&m_Bounces, sizeof(m_Bounces));
    mix(&m_RTScale, sizeof(m_RTScale));
    mix(&m_Denoise, sizeof(m_Denoise));
    mix(&m_DenoiseStrength, sizeof(m_DenoiseStrength));
    mix(&m_Aperture, sizeof(m_Aperture));
    bool ground = m_PathTracer.GroundPlane();
    mix(&ground, sizeof(ground));
    mix(&m_StlScale, sizeof(m_StlScale));
    return h;
}

bool EditorApp::SceneDirty() const
{
    return m_Commands.Revision() != m_SavedRevision || SettingsHash() != m_SavedSettingsHash;
}

void EditorApp::MarkSaved()
{
    m_SavedRevision = m_Commands.Revision();
    m_SavedSettingsHash = SettingsHash();
}

void EditorApp::DoNewScene()
{
    if (m_Sculpt.Active())
        m_Sculpt.Exit();
    m_Extrude.Disarm();
    m_Scene.Entities().clear();
    SelectOnly(0);
    m_Commands.Clear();
    m_ScenePath.clear();

    // A new scene must not inherit the previous scene's environment/settings —
    // reset everything BuildExtrasJson serializes.
    m_Env.reset();
    m_EnvPath.clear();
    m_Sun = DirectionalLight{};
    m_SunAzimuth = 40.0f;
    m_SunElevation = 50.0f;
    m_RayTracing = false;
    m_Bounces = 4;
    m_RTScale = 0.75f;
    m_Denoise = true;
    m_DenoiseStrength = 0.7f;
    m_Aperture = 0.0f;
    m_FocusDist = -1.0f;
    m_PathTracer.SetGroundPlane(true);
    for (CameraBookmark& b : m_Bookmarks)
        b.set = false;
    m_Camera.SetOrthographic(false);
    m_Camera.ApplyBookmark({{0.0f, 0.5f, 0.0f}, 8.0f, 0.45f, 0.65f});
    m_SpawnCounter = 1;
    m_StlScale = 100.0f;

    MarkSaved();
    m_LastSceneHash = 0; // force RT re-upload
}

void EditorApp::OpenSceneFile(const std::string& path)
{
    if (m_Sculpt.Active())
        m_Sculpt.Exit();
    m_Extrude.Disarm();

    std::string extras;
    auto fromRecipe = [this](const std::string& r) { return MeshFromRecipe(r); };
    if (!LoadSceneFile(path, m_Scene, extras, fromRecipe)) {
        FORGE_ERROR("Could not open scene: %s", path.c_str());
        return;
    }
    SelectOnly(0);
    m_Commands.Clear();
    ApplyExtrasJson(extras);
    m_ScenePath = path;
    MarkSaved();
    AddRecentFile(path);
    m_LastSceneHash = 0; // force RT re-upload
}

bool EditorApp::SaveScene()
{
    if (m_ScenePath.empty())
        return SaveSceneAs();
    auto toRecipe = [this](const Mesh* m) { return MeshRecipe(m); };
    if (!SaveSceneFile(m_ScenePath, m_Scene, BuildExtrasJson(), toRecipe)) {
        FORGE_ERROR("Save failed: %s", m_ScenePath.c_str());
        return false;
    }
    MarkSaved();
    AddRecentFile(m_ScenePath);
    return true;
}

bool EditorApp::SaveSceneAs()
{
    std::string path = SaveFileDialog(m_Window.NativeHandle(), "Forge Scene\0*.forge\0", "forge");
    if (path.empty())
        return false;
    std::string previous = m_ScenePath; // only adopt the new path once the write lands
    m_ScenePath = path;
    if (!SaveScene()) {
        m_ScenePath = previous;
        return false;
    }
    return true;
}

void EditorApp::RequestWithUnsavedCheck(FileAction action, const std::string& openPath)
{
    m_PendingAction = action;
    m_PendingOpenPath = openPath;
    if (SceneDirty()) {
        m_ShowUnsavedModal = true;
        return;
    }
    ExecutePendingAction();
}

void EditorApp::ExecutePendingAction()
{
    FileAction action = m_PendingAction;
    std::string openPath = m_PendingOpenPath;
    m_PendingAction = FileAction::None;
    m_PendingOpenPath.clear();

    switch (action) {
    case FileAction::NewScene:
        DoNewScene();
        break;
    case FileAction::OpenScene: {
        if (openPath.empty())
            openPath = OpenFileDialog(m_Window.NativeHandle(), "Forge Scene\0*.forge\0All Files\0*.*\0");
        if (!openPath.empty())
            OpenSceneFile(openPath);
        break;
    }
    case FileAction::Exit:
        m_ForceClose = true;
        m_Window.SetShouldClose(true);
        break;
    case FileAction::None:
        break;
    }
}

void EditorApp::DrawMainMenuBar()
{
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New", "Ctrl+N"))
                RequestWithUnsavedCheck(FileAction::NewScene);
            if (ImGui::MenuItem("Open...", "Ctrl+O"))
                RequestWithUnsavedCheck(FileAction::OpenScene);
            if (ImGui::BeginMenu("Open Recent", !m_RecentFiles.empty())) {
                // Iterate a copy: opening mutates the recents list.
                for (const std::string& path : std::vector<std::string>(m_RecentFiles))
                    if (ImGui::MenuItem(path.c_str()))
                        RequestWithUnsavedCheck(FileAction::OpenScene, path);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S"))
                SaveScene();
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
                SaveSceneAs();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
                RequestWithUnsavedCheck(FileAction::Exit);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    if (m_ShowUnsavedModal) {
        ImGui::OpenPopup("Unsaved Changes");
        m_ShowUnsavedModal = false;
    }
    if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::string name = m_ScenePath.empty()
                               ? "Untitled"
                               : std::filesystem::path(m_ScenePath).filename().string();
        ImGui::Text("Save changes to %s?", name.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Save", ImVec2(110, 0))) {
            ImGui::CloseCurrentPopup();
            if (SaveScene())
                ExecutePendingAction();
            else
                m_PendingAction = FileAction::None; // save canceled/failed: stay put
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(110, 0))) {
            ImGui::CloseCurrentPopup();
            ExecutePendingAction();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(110, 0))) {
            ImGui::CloseCurrentPopup();
            m_PendingAction = FileAction::None;
            m_PendingOpenPath.clear();
        }
        ImGui::EndPopup();
    }
}

void EditorApp::UpdateWindowTitle()
{
    std::string name = m_ScenePath.empty()
                           ? "Untitled"
                           : std::filesystem::path(m_ScenePath).filename().string();
    std::string title = name + (SceneDirty() ? "*" : "") + " - Forge Editor";
    if (title != m_LastTitle) {
        m_Window.SetTitle(title);
        m_LastTitle = title;
    }
}

static std::filesystem::path RecentFilesPath()
{
    const char* base = std::getenv("LOCALAPPDATA");
    std::filesystem::path dir = base ? std::filesystem::path(base) / "Forge"
                                     : std::filesystem::temp_directory_path() / "Forge";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir / "recent.txt";
}

void EditorApp::AddRecentFile(const std::string& path)
{
    m_RecentFiles.erase(std::remove(m_RecentFiles.begin(), m_RecentFiles.end(), path),
                        m_RecentFiles.end());
    m_RecentFiles.insert(m_RecentFiles.begin(), path);
    if (m_RecentFiles.size() > 8)
        m_RecentFiles.resize(8);
    SaveRecentFiles();
}

void EditorApp::LoadRecentFiles()
{
    std::ifstream in(RecentFilesPath());
    std::string line;
    while (std::getline(in, line))
        if (!line.empty())
            m_RecentFiles.push_back(line);
}

void EditorApp::SaveRecentFiles() const
{
    std::ofstream out(RecentFilesPath(), std::ios::trunc);
    for (const std::string& p : m_RecentFiles)
        out << p << "\n";
}

void EditorApp::ImportModel()
{
    std::string path = OpenFileDialog(m_Window.NativeHandle(),
                                      "3D Models (glTF, OBJ)\0*.gltf;*.glb;*.obj\0All Files\0*.*\0");
    if (!path.empty())
        ImportModel(path);
}

void EditorApp::ImportModel(const std::string& path)
{
    const std::vector<ImportedPart>* parts = AssetManager::Get().LoadModel(path);
    if (!parts) {
        FORGE_ERROR("Import failed: %s", path.c_str());
        return;
    }

    // Combined bounds of all parts (vertices already share the model's space).
    AABB bounds;
    for (const ImportedPart& p : *parts) {
        bounds.Expand(p.mesh->Bounds().min);
        bounds.Expand(p.mesh->Bounds().max);
    }
    vec3 extent = bounds.max - bounds.min;
    float maxExtent = std::max({extent.x, extent.y, extent.z});
    // Normalize wildly-sized free assets to a comfortable ~2.5 units.
    float s = maxExtent > 1e-6f ? 2.5f / maxExtent : 1.0f;
    vec3 center = (bounds.min + bounds.max) * 0.5f;
    vec3 focal = m_Camera.FocalPoint();
    vec3 t{focal.x - s * center.x, -s * bounds.min.y, focal.z - s * center.z}; // bottom on the ground

    auto composite = std::make_unique<CompositeCommand>();
    UUID selectId = 0;

    // Multi-part models import as one group: the root carries the normalize
    // transform, parts are identity-local children that move as a unit.
    UUID rootId = 0;
    if (parts->size() > 1) {
        Entity& root = m_Scene.CreateEntity(std::filesystem::path(path).stem().string());
        root.transform.translation = t;
        root.transform.scale = vec3(s);
        rootId = root.id;
        selectId = root.id;
        composite->Add(std::make_unique<AddEntityCommand>(root));
    }

    for (const ImportedPart& p : *parts) {
        Entity& e = m_Scene.CreateEntity(p.name);
        e.mesh = p.mesh;
        e.material = p.material;
        if (rootId) {
            e.parent = rootId; // local identity: mesh data already shares the model's space
        } else {
            e.transform.scale = vec3(s);
            e.transform.translation = t;
            selectId = e.id;
        }
        composite->Add(std::make_unique<AddEntityCommand>(e));
    }
    SelectOnly(selectId);
    m_Commands.Push(std::move(composite));
}

void EditorApp::MirrorSelected()
{
    Entity* e = m_Scene.Find(m_Selected);
    if (!e || !e->mesh)
        return;
    if (m_Sculpt.Active())
        m_Sculpt.Exit(); // topology is about to change under the sculpt tool

    int crossing = 0;
    for (const Vertex& v : e->mesh->Vertices())
        if (v.position.x < -1e-4f)
            ++crossing;
    if (crossing)
        FORGE_WARN("Mirror X: %d vertices already at x<0 kept as-is (no clipping in v1)", crossing);

    std::shared_ptr<Mesh> before = e->mesh;
    std::shared_ptr<Mesh> after = MirrorBakeX(*before);
    e->mesh = after; // pointer change bumps the RT scene hash automatically
    m_Commands.Push(std::make_unique<MeshSwapCommand>(e->id, before, after));
}

void EditorApp::SubdivideSelected(bool keepShape)
{
    Entity* e = m_Scene.Find(m_Selected);
    if (!e || !e->mesh)
        return;
    if (m_Sculpt.Active())
        m_Sculpt.Exit(); // topology is about to change under the sculpt tool

    std::shared_ptr<Mesh> before = e->mesh;
    std::shared_ptr<Mesh> after = LoopSubdivide(*before, keepShape);
    e->mesh = after;
    m_Commands.Push(std::make_unique<MeshSwapCommand>(e->id, before, after));
    FORGE_INFO("Subdivide (%s): %zu -> %zu tris", keepShape ? "keep shape" : "smooth",
               before->Indices().size() / 3, after->Indices().size() / 3);
}

void EditorApp::DropSelectedToGround()
{
    if (m_Selection.empty())
        return;

    auto worldAABB = [this](UUID node) {
        AABB box;
        Entity* e = m_Scene.Find(node);
        if (!e || !e->mesh)
            return box;
        mat4 world = m_Scene.WorldTransform(node);
        const AABB& lb = e->mesh->Bounds();
        for (int c = 0; c < 8; ++c) {
            vec3 corner{(c & 1) ? lb.max.x : lb.min.x, (c & 2) ? lb.max.y : lb.min.y,
                        (c & 4) ? lb.max.z : lb.min.z};
            box.Expand(vec3(world * vec4(corner, 1.0f)));
        }
        return box;
    };

    auto composite = std::make_unique<CompositeCommand>();
    // Sequential: dropping a multi-selection one root at a time lets later
    // objects land on earlier ones (a loose stack settles into a stack).
    for (UUID id : m_Selection) {
        Entity* root = m_Scene.Find(id);
        if (!root)
            continue;
        std::vector<UUID> subtree = SubtreeOf(id);

        AABB box; // groups move as one unit: combined AABB of the subtree
        for (UUID node : subtree) {
            AABB nb = worldAABB(node);
            if (nb.Valid()) {
                box.Expand(nb.min);
                box.Expand(nb.max);
            }
        }
        if (!box.Valid())
            continue; // nothing meshy under this root

        std::vector<AABB> supports;
        for (const Entity& e : m_Scene.Entities()) {
            if (!e.mesh || e.light.enabled) // light gizmos are not surfaces
                continue;
            if (std::find(subtree.begin(), subtree.end(), e.id) != subtree.end())
                continue;
            AABB sb = worldAABB(e.id);
            if (sb.Valid())
                supports.push_back(sb);
        }

        float dy = DropOffsetY(box, supports);
        if (std::abs(dy) < 1e-6f)
            continue; // already resting

        Entity before = *root;
        // The offset is world-space; a parented root needs it in parent space.
        mat4 parentWorld = m_Scene.WorldTransform(root->parent);
        root->transform.translation += vec3(glm::inverse(parentWorld) * vec4(0.0f, dy, 0.0f, 0.0f));
        composite->Add(std::make_unique<EditEntityCommand>(before, *root));
    }
    if (!composite->Empty())
        m_Commands.Push(std::move(composite));
}

void EditorApp::RemeshSelected()
{
    Entity* e = m_Scene.Find(m_Selected);
    if (!e || !e->mesh)
        return;
    if (m_Sculpt.Active())
        m_Sculpt.Exit();

    std::shared_ptr<Mesh> before = e->mesh;
    std::shared_ptr<Mesh> after = VoxelRemesh(*before, m_RemeshDetail);
    if (!after) {
        FORGE_ERROR("Remesh failed (degenerate or empty mesh)");
        return;
    }
    e->mesh = after;
    m_Commands.Push(std::make_unique<MeshSwapCommand>(e->id, before, after));
}

void EditorApp::BooleanSelected(BooleanOp op)
{
    if (m_Selection.size() != 2)
        return;
    Entity* pa = m_Scene.Find(m_Selection[0]);
    Entity* pb = m_Scene.Find(m_Selection[1]);
    if (!pa || !pb || !pa->mesh || !pb->mesh)
        return;
    if (m_Sculpt.Active())
        m_Sculpt.Exit();

    // Snapshot by value: CreateEntity/Remove below invalidate entity pointers.
    Entity ea = *pa, eb = *pb;
    BooleanResult r = MeshBoolean(*ea.mesh, m_Scene.WorldTransform(ea.id), *eb.mesh,
                                  m_Scene.WorldTransform(eb.id), op);
    if (!r.mesh) {
        m_BoolStatus = r.error;
        return;
    }
    m_BoolStatus.clear();

    auto composite = std::make_unique<CompositeCommand>();
    composite->Add(std::make_unique<DeleteEntityCommand>(ea));
    m_Scene.Remove(ea.id);
    composite->Add(std::make_unique<DeleteEntityCommand>(eb));
    m_Scene.Remove(eb.id);

    const char* opName = op == BooleanOp::Union      ? "Union"
                         : op == BooleanOp::Subtract ? "Subtract"
                                                     : "Intersect";
    Entity& result = m_Scene.CreateEntity(std::string(opName) + " " + std::to_string(m_SpawnCounter++));
    result.mesh = r.mesh;          // world-space baked
    result.material = ea.material; // first object's look carries over
    composite->Add(std::make_unique<AddEntityCommand>(result));
    UUID resultId = result.id;

    m_Commands.Push(std::move(composite));
    SelectOnly(resultId);
}

void EditorApp::StartTurntableDialog()
{
    std::string path = SaveFileDialog(m_Window.NativeHandle(), "GIF animation\0*.gif\0", "gif");
    if (!path.empty())
        StartTurntable(path, 48, 128);
}

bool EditorApp::StartTurntable(const std::string& path, int frames, int sppTarget)
{
    if (m_Turntable.active)
        return false;
    if (m_Sculpt.Active())
        m_Sculpt.Exit();

    // Fresh upload: the user may never have toggled RT on this session.
    m_PathTracer.Resize((uint32_t)(m_ViewportSize.x * m_RTScale), (uint32_t)(m_ViewportSize.y * m_RTScale));
    m_PathTracer.Upload(m_Scene);
    m_RTUploadPending = false;
    m_LastSceneHash = 0; // force re-upload when normal RT resumes afterwards
    GatherLights();

    TurntableJob& job = m_Turntable;
    job = {};
    job.totalFrames = frames;
    job.sppTarget = sppTarget;
    job.restore = m_Camera.GetBookmark();
    job.baseYaw = job.restore.yaw;
    job.pitch = job.restore.pitch;
    job.writer = new ForgeGifWriter{};
    if (!GifBegin(&job.writer->w, path.c_str(), m_PathTracer.Width(), m_PathTracer.Height(), 4)) {
        FORGE_ERROR("Turntable: could not open %s for writing", path.c_str());
        delete job.writer;
        job.writer = nullptr;
        return false;
    }
    job.active = true;
    FORGE_INFO("Turntable: %d frames at %ux%u, %d spp -> %s", frames, m_PathTracer.Width(),
               m_PathTracer.Height(), sppTarget, path.c_str());
    return true;
}

void EditorApp::UpdateTurntable()
{
    TurntableJob& job = m_Turntable;
    if (!job.active)
        return;

    if (job.sppDone == 0) { // new frame: spin the camera, restart accumulation
        m_Camera.SetOrbit(job.pitch,
                          job.baseYaw + glm::two_pi<float>() * (float)job.frame / (float)job.totalFrames);
        m_PathTracer.ResetAccumulation();
    }

    if (m_FocusDist <= 0.0f)
        m_FocusDist = glm::length(m_Camera.FocalPoint() - m_Camera.Position());
    const mat4& view = m_Camera.View();
    m_PathTracer.SetLens(m_Camera.IsOrthographic() ? 0.0f : m_Aperture, m_FocusDist,
                         vec3(view[0][0], view[1][0], view[2][0]), vec3(view[0][1], view[1][1], view[2][1]));
    m_PathTracer.SetDenoise(m_Denoise, m_DenoiseStrength);

    // ~8 spp per UI frame keeps the app responsive while converging.
    m_PathTracer.Dispatch(m_Camera.ViewProjection(), m_Camera.Position(), m_Sun, m_Bounces, m_FrameLights,
                          m_Env.get(), 8);
    job.sppDone += 8;
    if (job.sppDone < job.sppTarget)
        return;

    // Frame converged: read back the display image (GL is bottom-up; flip rows).
    uint32_t w = m_PathTracer.Width(), h = m_PathTracer.Height();
    std::vector<uint8_t> pixels((size_t)w * h * 4), flipped((size_t)w * h * 4);
    glBindTexture(GL_TEXTURE_2D, m_PathTracer.DisplayTexture());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    for (uint32_t y = 0; y < h; ++y)
        std::memcpy(&flipped[(size_t)y * w * 4], &pixels[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
    GifWriteFrame(&job.writer->w, flipped.data(), w, h, 4);

    job.sppDone = 0;
    if (++job.frame >= job.totalFrames)
        FinishTurntable();
}

void EditorApp::FinishTurntable()
{
    TurntableJob& job = m_Turntable;
    if (!job.active)
        return;
    GifEnd(&job.writer->w); // canceling keeps the partial GIF — frames written so far still play
    delete job.writer;
    job.writer = nullptr;
    job.active = false;
    m_Camera.ApplyBookmark(job.restore);
    FORGE_INFO("Turntable: wrote %d frames", job.frame);
}

void EditorApp::DrawTurntableModal()
{
    if (m_Turntable.active)
        ImGui::OpenPopup("Rendering Turntable");
    if (ImGui::BeginPopupModal("Rendering Turntable", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        TurntableJob& job = m_Turntable;
        float progress = job.active
                             ? ((float)job.frame + (float)job.sppDone / (float)job.sppTarget) / (float)job.totalFrames
                             : 1.0f;
        ImGui::ProgressBar(progress, ImVec2(280, 0));
        ImGui::Text("Frame %d / %d", std::min(job.frame + 1, job.totalFrames), job.totalFrames);
        if (ImGui::Button("Cancel", ImVec2(-1, 0)))
            FinishTurntable();
        if (!job.active)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void EditorApp::ExportStlDialog()
{
    std::string path = SaveFileDialog(m_Window.NativeHandle(), "STL (3D printing)\0*.stl\0", "stl");
    if (path.empty())
        return;

    // Selection (with subtrees) when present, whole scene otherwise.
    std::vector<UUID> ids;
    if (!m_Selection.empty()) {
        for (UUID id : m_Selection)
            for (UUID node : SubtreeOf(id))
                if (std::find(ids.begin(), ids.end(), node) == ids.end())
                    ids.push_back(node);
    } else {
        for (const Entity& e : m_Scene.Entities())
            ids.push_back(e.id);
    }

    StlExportResult r = ExportStl(m_Scene, ids, path, m_StlScale);
    if (!r.ok) {
        m_StlStatus = r.error;
        return;
    }
    char buf[192];
    if (r.watertight)
        std::snprintf(buf, sizeof(buf), "Exported %u triangles - watertight, print-ready.", r.triangles);
    else
        std::snprintf(buf, sizeof(buf),
                      "Exported %u triangles - NOT watertight (%u open, %u flipped edges). "
                      "A slicer may need to repair it.",
                      r.triangles, r.openEdges, r.flippedEdges);
    m_StlStatus = buf;
}

void EditorApp::DeleteSelected()
{
    if (m_Selection.empty())
        return;
    if (m_Sculpt.Active())
        m_Sculpt.Exit();

    auto composite = std::make_unique<CompositeCommand>();
    std::vector<UUID> selection = m_Selection; // ops mutate selection state
    for (UUID id : selection) {
        for (UUID node : SubtreeOf(id)) { // deleting a parent deletes the subtree
            if (Entity* e = m_Scene.Find(node)) {
                composite->Add(std::make_unique<DeleteEntityCommand>(*e));
                m_Scene.Remove(node);
            }
        }
    }
    if (!composite->Empty())
        m_Commands.Push(std::move(composite));
    SelectOnly(0);
}

void EditorApp::DuplicateSelected()
{
    if (m_Selection.empty())
        return;

    auto composite = std::make_unique<CompositeCommand>();
    UUID lastCopy = 0;
    for (UUID id : std::vector<UUID>(m_Selection)) {
        // Deep-copy the subtree with fresh ids, remapping parent links inside it.
        std::unordered_map<UUID, UUID> remap;
        for (UUID node : SubtreeOf(id)) {
            Entity* src = m_Scene.Find(node);
            if (!src)
                continue;
            Entity copy = *src;
            copy.id = GenerateUUID();
            remap[node] = copy.id;
            if (auto it = remap.find(copy.parent); it != remap.end())
                copy.parent = it->second; // internal link
            if (node == id) {
                copy.name += " copy";
                copy.transform.translation += vec3(0.5f, 0.0f, 0.5f);
            }
            m_Scene.Insert(copy);
            composite->Add(std::make_unique<AddEntityCommand>(copy));
            if (node == id)
                lastCopy = copy.id;
        }
    }
    if (!composite->Empty()) {
        m_Commands.Push(std::move(composite));
        SelectOnly(lastCopy);
    }
}

void EditorApp::GroupSelection()
{
    // Group the topmost selected entities (skip anything whose ancestor is also selected).
    std::vector<UUID> roots;
    for (UUID id : m_Selection) {
        bool covered = false;
        for (UUID other : m_Selection)
            if (other != id && m_Scene.IsDescendantOf(id, other))
                covered = true;
        if (!covered && m_Scene.Find(id))
            roots.push_back(id);
    }
    if (roots.empty())
        return;

    vec3 centroid(0.0f);
    for (UUID id : roots)
        centroid += vec3(m_Scene.WorldTransform(id)[3]);
    centroid /= (float)roots.size();

    Entity& group = m_Scene.CreateEntity("Group " + std::to_string(m_SpawnCounter++));
    group.transform.translation = centroid; // identity rotation/scale: keeps child TRS decomposable

    auto composite = std::make_unique<CompositeCommand>();
    composite->Add(std::make_unique<AddEntityCommand>(group));
    UUID groupId = group.id;
    for (UUID id : roots) {
        Entity before = *m_Scene.Find(id);
        m_Scene.ReparentKeepWorld(id, groupId);
        composite->Add(std::make_unique<EditEntityCommand>(before, *m_Scene.Find(id)));
    }
    m_Commands.Push(std::move(composite));
    SelectOnly(groupId);
}

void EditorApp::UngroupSelected()
{
    Entity* group = m_Scene.Find(m_Selected);
    if (!group)
        return;
    std::vector<UUID> children = m_Scene.ChildrenOf(group->id);
    if (children.empty())
        return;

    auto composite = std::make_unique<CompositeCommand>();
    UUID newParent = group->parent;
    for (UUID child : children) {
        Entity before = *m_Scene.Find(child);
        m_Scene.ReparentKeepWorld(child, newParent);
        composite->Add(std::make_unique<EditEntityCommand>(before, *m_Scene.Find(child)));
    }
    // Delete the (now childless) group only if it's a pure container.
    if (!group->mesh && !group->light.enabled) {
        composite->Add(std::make_unique<DeleteEntityCommand>(*group));
        m_Scene.Remove(group->id);
        SelectOnly(0);
    }
    m_Commands.Push(std::move(composite));
}

Ray EditorApp::ViewportRay(const vec2& uv) const
{
    vec4 ndcNear{uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, -1.0f, 1.0f};
    vec4 ndcFar{ndcNear.x, ndcNear.y, 1.0f, 1.0f};
    mat4 invVP = glm::inverse(m_Camera.ViewProjection());
    vec4 pNear = invVP * ndcNear;
    vec4 pFar = invVP * ndcFar;
    pNear /= pNear.w;
    pFar /= pFar.w;
    return Ray{vec3(pNear), glm::normalize(vec3(pFar - pNear))};
}

// CollapsingHeader with the larger header font.
static bool Section(ImFont* headerFont, const char* label, bool defaultOpen)
{
    ImGui::PushFont(headerFont);
    bool open = ImGui::CollapsingHeader(label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
    ImGui::PopFont();
    return open;
}

void EditorApp::DrawSidebar()
{
    ImGui::Begin("Tools");
    const float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    if (Section(m_HeaderFont, "Create", true)) {
        auto shapeBtn = [&](const char* name, const std::shared_ptr<Mesh>& mesh, float y, bool sameLine) {
            if (ImGui::Button(name, ImVec2(halfW, 32)))
                SpawnPrimitive(name, mesh, y);
            if (sameLine)
                ImGui::SameLine();
        };
        shapeBtn("Cube", m_CubeMesh, 0.5f, true);
        shapeBtn("Sphere", m_SphereMesh, 0.5f, false);
        shapeBtn("Cylinder", m_CylinderMesh, 0.5f, true);
        shapeBtn("Cone", m_ConeMesh, 0.5f, false);
        shapeBtn("Torus", m_TorusMesh, 0.15f, true);
        shapeBtn("Plane", m_PlaneMesh, 0.01f, false);

        if (ImGui::Button("Point Light", ImVec2(-1, 32)))
            SpawnPointLight();
        ImGui::SetItemTooltip("A small light you can place anywhere");
        if (ImGui::Button("Sculpt Sphere", ImVec2(-1, 32)))
            SpawnPrimitive("Sculpt Sphere", m_SculptSphereMesh, 0.5f);
        ImGui::SetItemTooltip("A high-detail sphere ready for sculpting");
        if (ImGui::Button("Terrain", ImVec2(-1, 32)))
            SpawnPrimitive("Terrain", m_TerrainMesh, 0.01f);
        ImGui::SetItemTooltip("A flat ground you can sculpt into hills");
        if (ImGui::Button("3D Text...", ImVec2(-1, 32)))
            ImGui::OpenPopup("Add 3D Text");
        ImGui::SetItemTooltip("Turn a word into a solid 3D object (great for nameplates)");
        if (ImGui::BeginPopupModal("Add 3D Text", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::InputText("Text", m_TextInput, sizeof(m_TextInput));
            ImGui::SliderFloat("Depth", &m_TextDepth, 0.05f, 1.0f);
            bool create = ImGui::Button("Create", ImVec2(120, 0)) && m_TextInput[0];
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0)))
                ImGui::CloseCurrentPopup();
            if (create) {
                if (auto mesh = MeshFactory::Text(m_TextInput, "C:/Windows/Fonts/segoeui.ttf", m_TextDepth))
                    SpawnPrimitive(m_TextInput, mesh, 0.5f);
                else
                    FORGE_ERROR("3D text failed (font missing or unsupported outline format)");
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ui::PushAccentButton();
        if (ImGui::Button("Import Model...", ImVec2(-1, 36)))
            ImportModel();
        ui::PopAccentButton();
        ImGui::SetItemTooltip("Bring in a downloaded 3D model (.gltf, .glb, .obj)");
    }

    if (Section(m_HeaderFont, "Sculpt", true)) {
        Entity* selEntity = m_Scene.Find(m_Selected);
        bool canSculpt = (selEntity && selEntity->mesh) || m_Sculpt.Active();
        ImGui::BeginDisabled(!canSculpt);
        if (m_Sculpt.Active()) {
            ui::PushAccentButton();
            if (ImGui::Button("Exit Sculpt (Tab)", ImVec2(-1, 32)))
                ToggleSculptMode();
            ui::PopAccentButton();
        } else {
            if (ImGui::Button("Sculpt Mode (Tab)", ImVec2(-1, 32)))
                ToggleSculptMode();
            ImGui::SetItemTooltip("Shape the selected object with brushes");
        }
        ImGui::EndDisabled();
        if (m_Sculpt.Active())
            m_Sculpt.DrawSettingsUI();
    }

    if (Section(m_HeaderFont, "Modify", false)) {
        Entity* sel = m_Scene.Find(m_Selected);
        bool canModify = sel && sel->mesh && !sel->light.enabled;
        size_t triCount = canModify ? sel->mesh->Indices().size() / 3 : 0;
        bool tooDense = triCount * 4 > 100000; // subdivision quadruples the count

        ImGui::BeginDisabled(m_Selection.empty());
        if (ImGui::Button("Drop to Ground", ImVec2(-1, 30)))
            DropSelectedToGround();
        ImGui::SetItemTooltip("Rest the selection on the floor or whatever is under it (End)");
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!canModify);
        if (ImGui::Button("Mirror X", ImVec2(-1, 30)))
            MirrorSelected();
        ImGui::SetItemTooltip("Bake a left-right mirrored copy across the object's own center plane");

        ImGui::BeginDisabled(tooDense);
        if (ImGui::Button("Subdivide", ImVec2(-1, 30)))
            SubdivideSelected(m_SubdivKeepShape);
        ImGui::SetItemTooltip("Split every triangle into four for finer detail");
        ImGui::EndDisabled();
        ImGui::Checkbox("Keep shape", &m_SubdivKeepShape);
        ImGui::SetItemTooltip("On: only add resolution (for sculpting).\nOff: also smooth the surface rounder");
        if (tooDense)
            ImGui::TextDisabled("Too dense to subdivide (%zu tris)", triCount);

        if (ImGui::Button("Remesh", ImVec2(-1, 30)))
            RemeshSelected();
        ImGui::SetItemTooltip("Rebuild the surface as clean, even triangles.\nFixes stretched or messy geometry before sculpting");
        ImGui::SliderInt("Detail", &m_RemeshDetail, 32, 160);
        ImGui::SetItemTooltip("Higher keeps more detail but makes more triangles");
        ImGui::EndDisabled();
        if (!canModify)
            ImGui::TextDisabled("Select an object to modify.");

        // Booleans need exactly two solid meshes.
        bool canBool = m_Selection.size() == 2;
        for (UUID id : m_Selection) {
            Entity* be = m_Scene.Find(id);
            if (!be || !be->mesh || be->light.enabled)
                canBool = false;
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Boolean");
        ImGui::SetItemTooltip("Combine or carve two objects (select both with Ctrl+click)");
        ImGui::BeginDisabled(!canBool);
        float thirdW = (ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
        if (ImGui::Button("Union", ImVec2(thirdW, 28)))
            BooleanSelected(BooleanOp::Union);
        ImGui::SetItemTooltip("Merge both objects into one solid");
        ImGui::SameLine();
        if (ImGui::Button("Subtract", ImVec2(thirdW, 28)))
            BooleanSelected(BooleanOp::Subtract);
        ImGui::SetItemTooltip("Carve the second-selected object out of the first");
        ImGui::SameLine();
        if (ImGui::Button("Intersect", ImVec2(thirdW, 28)))
            BooleanSelected(BooleanOp::Intersect);
        ImGui::SetItemTooltip("Keep only where both objects overlap");
        ImGui::EndDisabled();
        if (!canBool)
            ImGui::TextDisabled("Select exactly two objects (Ctrl+click).");
        if (!m_BoolStatus.empty())
            ImGui::TextWrapped("%s", m_BoolStatus.c_str());

        ImGui::Spacing();
        bool armed = m_Extrude.Armed();
        if (armed)
            ui::PushAccentButton();
        if (ImGui::Button(armed ? "Extrude: drag a face..." : "Extrude (push/pull)", ImVec2(-1, 30))) {
            if (armed)
                m_Extrude.Disarm();
            else if (!m_Sculpt.Active())
                m_Extrude.Arm();
        }
        if (armed)
            ui::PopAccentButton();
        ImGui::SetItemTooltip("Pull a flat face out or push it in.\nClick the button, then press and drag a face in the viewport");
    }

    if (Section(m_HeaderFont, "Lighting & Sky", false)) {
        ImGui::SliderFloat("Sun direction", &m_SunAzimuth, 0.0f, 360.0f, "%.0f deg");
        ImGui::SetItemTooltip("Compass direction the sunlight comes from");
        ImGui::SliderFloat("Sun height", &m_SunElevation, 5.0f, 89.0f, "%.0f deg");
        ImGui::SetItemTooltip("Low = sunset, high = noon");
        ImGui::SliderFloat("Sun intensity", &m_Sun.intensity, 0.0f, 3.0f);
        ImGui::ColorEdit3("Sun color", &m_Sun.color.x);
        ImGui::Spacing();
        if (ImGui::Button("Load HDRI...", ImVec2(-1, 30)))
            LoadHDRI();
        ImGui::SetItemTooltip("A 360-degree photo used as the sky and lighting");
        if (m_Env && m_Env->Valid()) {
            ImGui::SliderFloat("Sky intensity", &m_Env->intensity, 0.0f, 4.0f);
            ImGui::SliderFloat("Sky rotation", &m_Env->rotationDegrees, 0.0f, 360.0f, "%.0f deg");
            if (ImGui::Button("Clear HDRI", ImVec2(-1, 0))) {
                m_Env.reset();
                m_EnvPath.clear();
            }
        }
    }

    if (Section(m_HeaderFont, "Display", false)) {
        int shading = (int)m_Renderer.GetShadingMode();
        if (ImGui::Combo("Shading", &shading, "Flat\0Simple (Blinn-Phong)\0Realistic (PBR)\0"))
            m_Renderer.SetShadingMode((ShadingMode)shading);
        bool shadows = m_Renderer.ShadowsEnabled();
        if (ImGui::Checkbox("Shadows", &shadows))
            m_Renderer.SetShadowsEnabled(shadows);
        float bloom = m_Post.BloomStrength();
        if (ImGui::SliderFloat("Bloom", &bloom, 0.0f, 0.3f))
            m_Post.SetBloomStrength(bloom);
        ImGui::SetItemTooltip("Makes bright things glow");
        float fov = m_Camera.FOV();
        if (ImGui::SliderFloat("Camera FOV", &fov, 15.0f, 100.0f, "%.0f deg"))
            m_Camera.SetFOV(fov);
        ImGui::SetItemTooltip("Camera lens angle - wide or zoomed");
        bool ortho = m_Camera.IsOrthographic();
        if (ImGui::Checkbox("Orthographic", &ortho))
            m_Camera.SetOrthographic(ortho);
        ImGui::SetItemTooltip("Parallel projection - no perspective, like a blueprint.\nGreat with the 1/3/7 view keys");

        // Transform snapping (#5): mirrors the viewport's Snap button; the step
        // pickers appear only when snapping is on to keep the panel uncluttered.
        if (ImGui::Checkbox("Snapping", &m_SnapEnabled))
            FORGE_INFO("Snapping %s (grid %.2f, angle %.0f deg)", m_SnapEnabled ? "on" : "off",
                       m_SnapTranslate, m_SnapRotateDeg);
        ImGui::SetItemTooltip("Snap gizmo drags and inspector fields to fixed steps.\nHold Ctrl to flip it for one drag");
        if (m_SnapEnabled) {
            static const float kGrid[] = {0.1f, 0.25f, 0.5f, 1.0f};
            int gi = m_SnapTranslate <= 0.15f ? 0 : m_SnapTranslate <= 0.35f ? 1 : m_SnapTranslate <= 0.75f ? 2 : 3;
            if (ImGui::Combo("Move step", &gi, "0.1\0" "0.25\0" "0.5\0" "1.0\0"))
                m_SnapTranslate = kGrid[gi];
            static const float kAngle[] = {5.0f, 15.0f, 45.0f};
            int ai = m_SnapRotateDeg <= 7.5f ? 0 : m_SnapRotateDeg <= 30.0f ? 1 : 2;
            if (ImGui::Combo("Rotate step", &ai, "5 deg\0" "15 deg\0" "45 deg\0"))
                m_SnapRotateDeg = kAngle[ai];
        }
    }

    if (Section(m_HeaderFont, "Ray Tracing (photoreal)", false)) {
        ImGui::Checkbox("Enable", &m_RayTracing);
        ImGui::SetItemTooltip("Physically accurate light - slower, sharpens over time");
        if (m_RayTracing) {
            int quality = m_RTScale <= 0.55f ? 0 : (m_RTScale <= 0.80f ? 1 : 2);
            if (ImGui::Combo("Quality", &quality, "Fast (50%)\0Balanced (75%)\0Full (100%)\0"))
                m_RTScale = quality == 0 ? 0.5f : (quality == 1 ? 0.75f : 1.0f);
            ImGui::SetItemTooltip("Render resolution - lower converges much faster");
            ImGui::SliderInt("Bounces", &m_Bounces, 1, 8);
            ImGui::SetItemTooltip("Light bounces. 3-4 is usually enough;\nglass and water want 6+ to see through.\nMore adds noise faster than realism");
            bool ground = m_PathTracer.GroundPlane();
            if (ImGui::Checkbox("Ground plane", &ground))
                m_PathTracer.SetGroundPlane(ground);
            ImGui::SetItemTooltip("A studio floor that catches shadows");

            ImGui::Checkbox("Denoise", &m_Denoise);
            ImGui::SetItemTooltip("Smooths the grainy noise away while the image converges");
            if (m_Denoise) {
                ImGui::SliderFloat("Denoise strength", &m_DenoiseStrength, 0.0f, 1.0f);
                ImGui::SetItemTooltip("Higher = smoother but softer early on");
            }

            ImGui::SliderFloat("Aperture", &m_Aperture, 0.0f, 0.3f, "%.3f");
            ImGui::SetItemTooltip("Camera lens size. 0 = everything sharp;\nbigger = blurrier foreground/background");
            if (m_Aperture > 0.0f) {
                if (m_Camera.IsOrthographic()) {
                    ImGui::TextDisabled("Depth of field is off in orthographic view");
                } else {
                    if (m_FocusDist <= 0.0f)
                        m_FocusDist = glm::length(m_Camera.FocalPoint() - m_Camera.Position());
                    ImGui::SliderFloat("Focus distance", &m_FocusDist, 0.5f, 50.0f, "%.2f");
                    ImGui::SetItemTooltip("Distance to the plane that stays sharp");
                    if (ImGui::Button("Focus on selected", ImVec2(-1, 0))) {
                        if (Entity* sel = m_Scene.Find(m_Selected))
                            m_FocusDist = glm::length(vec3(m_Scene.WorldTransform(sel->id)[3]) -
                                                      m_Camera.Position());
                    }
                    ImGui::SetItemTooltip("Set the focus distance to the selected object");
                }
            }

            ImGui::TextDisabled("%d samples, %zu triangles", m_PathTracer.SampleCount(),
                                m_PathTracer.TriangleCount());
        }
    }

    if (Section(m_HeaderFont, "Export", false)) {
        ImGui::DragFloat("Scale (mm/unit)", &m_StlScale, 1.0f, 1.0f, 1000.0f, "%.0f");
        ImGui::SetItemTooltip("How many millimeters one scene unit becomes when printed");
        if (ImGui::Button("Export STL...", ImVec2(-1, 30)))
            ExportStlDialog();
        ImGui::SetItemTooltip("Save for 3D printing. Exports the selection,\nor the whole scene when nothing is selected");
        if (!m_StlStatus.empty())
            ImGui::TextWrapped("%s", m_StlStatus.c_str());

        ImGui::Spacing();
        ImGui::BeginDisabled(m_Turntable.active || m_Scene.Entities().empty());
        if (ImGui::Button("Turntable GIF...", ImVec2(-1, 30)))
            StartTurntableDialog();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Render a looping 360-degree spin of your scene\n(path traced - takes a minute)");
    }

    DrawTurntableModal();

    ImGui::End();
}

void EditorApp::DrawHierarchyNode(Entity& e)
{
    std::vector<UUID> children = m_Scene.ChildrenOf(e.id);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (children.empty())
        flags |= ImGuiTreeNodeFlags_Leaf;
    if (IsSelected(e.id))
        flags |= ImGuiTreeNodeFlags_Selected;

    bool open = ImGui::TreeNodeEx((void*)(uintptr_t)e.id, flags, "%s", e.name.c_str());

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) {
        if (ImGui::GetIO().KeyCtrl)
            ToggleSelection(e.id);
        else
            SelectOnly(e.id);
    }

    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) { SelectOnly(e.id); DuplicateSelected(); }
        if (ImGui::MenuItem("Delete", "Del")) { SelectOnly(e.id); DeleteSelected(); }
        if (e.parent != 0 && ImGui::MenuItem("Unparent")) {
            Entity before = e;
            m_Scene.ReparentKeepWorld(e.id, 0);
            m_Commands.Push(std::make_unique<EditEntityCommand>(before, *m_Scene.Find(before.id)));
        }
        ImGui::EndPopup();
    }

    // Drag-drop reparenting.
    if (ImGui::BeginDragDropSource()) {
        UUID id = e.id;
        ImGui::SetDragDropPayload("FORGE_ENTITY", &id, sizeof(id));
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FORGE_ENTITY")) {
            UUID dragged = *(const UUID*)payload->Data;
            if (dragged != e.id && !m_Scene.IsDescendantOf(e.id, dragged)) {
                Entity before = *m_Scene.Find(dragged);
                m_Scene.ReparentKeepWorld(dragged, e.id);
                m_Commands.Push(std::make_unique<EditEntityCommand>(before, *m_Scene.Find(dragged)));
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (open) {
        for (UUID child : children)
            if (Entity* c = m_Scene.Find(child))
                DrawHierarchyNode(*c);
        ImGui::TreePop();
    }
}

void EditorApp::DrawHierarchy()
{
    ImGui::Begin("Scene");

    // Toolbar: group + delete for the current selection.
    ImGui::BeginDisabled(m_Selection.empty());
    if (ImGui::Button("+ Group"))
        GroupSelection();
    ImGui::SetItemTooltip("Put selected objects in a folder (Ctrl+G)");
    ImGui::SameLine();
    ui::PushDangerButton();
    if (ImGui::Button("Delete"))
        DeleteSelected();
    ui::PopDangerButton();
    ImGui::SetItemTooltip("Delete selected (Del)");
    ImGui::EndDisabled();
    ImGui::Separator();

    if (m_Scene.Entities().empty())
        ImGui::TextDisabled("Your scene is empty -\nadd a shape from Tools.");

    // Iterate by id: tree interactions (delete/duplicate) mutate the entity vector.
    std::vector<UUID> roots;
    for (const Entity& e : m_Scene.Entities())
        if (e.parent == 0)
            roots.push_back(e.id);
    for (UUID id : roots)
        if (Entity* e = m_Scene.Find(id))
            DrawHierarchyNode(*e);

    // Empty space: click deselects, drop unparents.
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FORGE_ENTITY")) {
            UUID dragged = *(const UUID*)payload->Data;
            Entity before = *m_Scene.Find(dragged);
            m_Scene.ReparentKeepWorld(dragged, 0);
            m_Commands.Push(std::make_unique<EditEntityCommand>(before, *m_Scene.Find(dragged)));
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
        SelectOnly(0);

    ImGui::End();
}

void EditorApp::DrawInspector()
{
    ImGui::Begin("Inspector");

    Entity* e = m_Scene.Find(m_Selected);
    if (!e) {
        ImGui::TextDisabled("Nothing selected.\nClick an object in the viewport,\nor create one from the Tools panel.");
        ImGui::End();
        return;
    }

    // Snapshot on widget activation, push one undo entry when the edit ends.
    auto track = [&]() {
        if (ImGui::IsItemActivated())
            m_BeforeEdit = *e;
        if (ImGui::IsItemDeactivatedAfterEdit())
            m_Commands.Push(std::make_unique<EditEntityCommand>(m_BeforeEdit, *e));
    };
    auto sepText = [&](const char* label) {
        ImGui::PushFont(m_HeaderFont);
        ImGui::SeparatorText(label);
        ImGui::PopFont();
    };

    char nameBuf[128];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", e->name.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##name", "Object name", nameBuf, sizeof(nameBuf)))
        e->name = nameBuf;
    track();

    sepText("Transform");
    // Row of X/Y/Z colored badges (click = reset) + drag fields.
    auto vec3Row = [&](const char* label, vec3& v, float speed, float resetVal, bool* changed,
                       float snapStep) {
        static const ImVec4 axisColor[3] = {{0.757f, 0.27f, 0.24f, 1}, {0.37f, 0.63f, 0.23f, 1},
                                            {0.24f, 0.47f, 0.76f, 1}};
        static const char* axisName[3] = {"X", "Y", "Z"};
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        float fieldW =
            (ImGui::GetContentRegionAvail().x - 3 * (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x) -
             2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
        for (int a = 0; a < 3; ++a) {
            ImGui::PushID(a);
            if (a > 0)
                ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, axisColor[a]);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, axisColor[a]);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, axisColor[a]);
            if (ImGui::Button(axisName[a], ImVec2(ImGui::GetFrameHeight(), 0))) {
                Entity before = *e;
                (&v.x)[a] = resetVal;
                if (changed)
                    *changed = true;
                m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));
            }
            ImGui::PopStyleColor(3);
            ImGui::SetItemTooltip("Click to reset");
            ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
            ImGui::SetNextItemWidth(fieldW);
            float* comp = &(&v.x)[a];
            if (ImGui::DragFloat("##v", comp, speed, 0.0f, 0.0f, "%.2f")) {
                if (snapStep > 0.0f) // snapping on: clean numbers in the field too
                    *comp = SnapToStep(*comp, snapStep);
                if (changed)
                    *changed = true;
            }
            track();
            ImGui::PopID();
        }
        ImGui::PopID();
    };

    vec3Row("Position", e->transform.translation, 0.05f, 0.0f, nullptr,
            m_SnapEnabled ? m_SnapTranslate : 0.0f);
    vec3 deg = glm::degrees(e->transform.rotation);
    bool rotChanged = false;
    vec3Row("Rotation", deg, 0.5f, 0.0f, &rotChanged, m_SnapEnabled ? m_SnapRotateDeg : 0.0f);
    if (rotChanged)
        e->transform.rotation = glm::radians(deg);
    vec3Row("Scale", e->transform.scale, 0.02f, 1.0f, nullptr, m_SnapEnabled ? m_SnapScale : 0.0f);

    sepText("Material");
    ImGui::ColorEdit3("Albedo", &e->material.albedo.x);
    ImGui::SetItemTooltip("The object's base color");
    track();
    ImGui::SliderFloat("Metallic", &e->material.metallic, 0.0f, 1.0f);
    ImGui::SetItemTooltip("0 = plastic or wood, 1 = metal");
    track();
    ImGui::SliderFloat("Roughness", &e->material.roughness, 0.0f, 1.0f);
    ImGui::SetItemTooltip("0 = polished mirror, 1 = matte");
    track();
    ImGui::ColorEdit3("Emissive", &e->material.emissive.x);
    track();
    ImGui::SliderFloat("Emission", &e->material.emissiveStrength, 0.0f, 20.0f);
    ImGui::SetItemTooltip("Makes the object glow and light the scene");
    track();
    ImGui::SliderFloat("Transmission", &e->material.transmission, 0.0f, 1.0f);
    ImGui::SetItemTooltip("0 = solid, 1 = clear like glass or water");
    track();
    if (e->material.transmission > 0.0f) {
        ImGui::SliderFloat("IOR", &e->material.ior, 1.0f, 2.5f);
        ImGui::SetItemTooltip("How much light bends: water 1.33, glass 1.5, diamond 2.4");
        track();
    }

    // Preset buttons write fields programmatically, so they push their own undo
    // entry instead of relying on track()'s widget edit-state.
    auto preset = [&](const char* label, vec3 albedo, float metallic, float roughness, float transmission,
                      float ior) {
        if (ImGui::SmallButton(label)) {
            Entity before = *e;
            e->material.albedo = albedo;
            e->material.metallic = metallic;
            e->material.roughness = roughness;
            e->material.transmission = transmission;
            e->material.ior = ior;
            m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));
        }
    };
    ImGui::TextDisabled("Presets:");
    ImGui::SameLine();
    preset("Mirror", {0.95f, 0.95f, 0.95f}, 1.0f, 0.03f, 0.0f, 1.5f);
    ImGui::SameLine();
    preset("Water", {0.80f, 0.92f, 0.98f}, 0.0f, 0.02f, 1.0f, 1.33f);
    ImGui::SameLine();
    preset("Glass", {1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 1.0f, 1.5f);
    ImGui::SameLine();
    preset("Frosted", {1.0f, 1.0f, 1.0f}, 0.0f, 0.4f, 1.0f, 1.5f);

    if (e->light.enabled) {
        sepText("Point Light");
        if (ImGui::ColorEdit3("Light color", &e->light.color.x)) {
            e->material.albedo = e->light.color; // keep the gizmo sphere in sync
            e->material.emissive = e->light.color;
        }
        track();
        ImGui::SliderFloat("Intensity", &e->light.intensity, 0.0f, 200.0f);
        track();
        ImGui::SliderFloat("Range", &e->light.range, 0.5f, 100.0f);
        track();
    }
    if (e->material.albedoMap) {
        ImGui::Text("Albedo map: %ux%u", e->material.albedoMap->Width(), e->material.albedoMap->Height());
        if (ImGui::Button("Remove texture")) {
            Entity before = *e;
            e->material.albedoMap = nullptr;
            m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));
        }
    }

    ImGui::Spacing();
    float actionW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Duplicate", ImVec2(actionW, 30)))
        DuplicateSelected();
    ImGui::SetItemTooltip("Copy this object (Ctrl+D)");
    ImGui::SameLine();
    ui::PushDangerButton();
    if (ImGui::Button("Delete", ImVec2(actionW, 30)))
        DeleteSelected();
    ui::PopDangerButton();
    ImGui::SetItemTooltip("Delete this object (Del)");

    ImGui::End();
}

void EditorApp::DrawViewport()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");
    m_ViewportHovered = ImGui::IsWindowHovered();

    // Camera input here: ImGui IO is fresh, and the scene renders after the
    // UI pass with this frame's camera (no lag between gizmo and image).
    m_Camera.OnUpdate(m_ViewportHovered && !ImGuizmo::IsUsing());

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x > 0 && avail.y > 0) {
        m_Framebuffer.Resize((uint32_t)avail.x, (uint32_t)avail.y);
        m_Camera.SetViewportSize(avail.x, avail.y);
        // Sculpt mode and pending BVH rebuilds force raster rendering — show the
        // raster output too, not a stale path-traced frame.
        bool showRT = (m_RayTracing || m_Turntable.active) && !m_Sculpt.Active() && !m_RTUploadPending;
        uint32_t texture = showRT ? m_PathTracer.DisplayTexture() : m_DisplayTex;
        if (texture == 0)
            texture = m_Framebuffer.ColorAttachment(); // first frame before post runs
        ImGui::Image((ImTextureID)(uint64_t)texture, avail,
                     ImVec2(0, 1), ImVec2(1, 0)); // flip Y: GL textures are bottom-up

        ImVec2 imgMin = ImGui::GetItemRectMin();
        m_ViewportPos = {imgMin.x, imgMin.y};
        m_ViewportSize = {avail.x, avail.y};

        if (m_Sculpt.Active()) {
            // Sculpt mode owns the plain LMB; gizmo and selection are suppressed.
            if (auto cmd = m_Sculpt.OnViewportFrame(m_Scene, m_Camera, m_ViewportPos, m_ViewportSize,
                                                    m_ViewportHovered))
                m_Commands.Push(std::move(cmd));
            // Mode border: unmistakable "you are sculpting" signal.
            ImGui::GetWindowDrawList()->AddRect(imgMin, ImVec2(imgMin.x + avail.x, imgMin.y + avail.y),
                                                IM_COL32(240, 148, 56, 255), 0.0f, 0, 2.0f);
        } else if (m_Extrude.Busy()) {
            // Extrude owns the plain LMB while armed; gizmo and selection wait.
            if (auto cmd = m_Extrude.OnViewportFrame(m_Scene, m_Camera, m_ViewportPos, m_ViewportSize,
                                                     m_ViewportHovered))
                m_Commands.Push(std::move(cmd));
            ImGui::GetWindowDrawList()->AddRect(imgMin, ImVec2(imgMin.x + avail.x, imgMin.y + avail.y),
                                                IM_COL32(86, 156, 214, 255), 0.0f, 0, 2.0f);
        } else {
            // --- Gizmo (manipulates the primary selection through its parent chain) ---
            Entity* sel = m_Scene.Find(m_Selected);
            if (sel) {
                ImGuizmo::SetOrthographic(m_Camera.IsOrthographic());
                ImGuizmo::SetDrawlist();
                ImGuizmo::SetRect(m_ViewportPos.x, m_ViewportPos.y, m_ViewportSize.x, m_ViewportSize.y);

                ImGuizmo::OPERATION op = m_GizmoOp == GizmoOp::Translate ? ImGuizmo::TRANSLATE
                                       : m_GizmoOp == GizmoOp::Rotate    ? ImGuizmo::ROTATE
                                                                         : ImGuizmo::SCALE;
                mat4 parentWorld = m_Scene.WorldTransform(sel->parent);
                mat4 model = parentWorld * sel->transform.World();
                // Snapping: the toggle persists, holding Ctrl flips it for one
                // drag. ImGuizmo reads a per-axis step vector; the active step
                // depends on the operation (grid / angle / scale factor).
                bool snapActive = m_SnapEnabled != ImGui::GetIO().KeyCtrl;
                float step = m_GizmoOp == GizmoOp::Translate ? m_SnapTranslate
                           : m_GizmoOp == GizmoOp::Rotate    ? m_SnapRotateDeg
                                                             : m_SnapScale;
                float snap[3] = {step, step, step};
                ImGuizmo::Manipulate(glm::value_ptr(m_Camera.View()), glm::value_ptr(m_Camera.Projection()),
                                     op, ImGuizmo::LOCAL, glm::value_ptr(model), nullptr,
                                     snapActive ? snap : nullptr);

                if (ImGuizmo::IsUsing()) {
                    if (!m_GizmoWasUsing) {
                        m_GizmoWasUsing = true;
                        m_BeforeEdit = *sel;
                    }
                    mat4 local = glm::inverse(parentWorld) * model;
                    float t[3], r[3], s[3];
                    ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(local), t, r, s);
                    sel->transform.translation = {t[0], t[1], t[2]};
                    sel->transform.rotation = glm::radians(vec3(r[0], r[1], r[2]));
                    sel->transform.scale = {s[0], s[1], s[2]};
                } else if (m_GizmoWasUsing) {
                    m_GizmoWasUsing = false;
                    m_Commands.Push(std::make_unique<EditEntityCommand>(m_BeforeEdit, *sel));
                }
            }

            // --- Click-to-select (release with negligible drag = click, not orbit) ---
            ImGuiIO& io = ImGui::GetIO();
            bool clicked = ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                           !io.KeyAlt && // Alt+LMB belongs to the camera
                           io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 25.0f;
            if (clicked && !ImGuizmo::IsUsing() && !ImGuizmo::IsOver()) {
                ImVec2 mouse = ImGui::GetMousePos();
                vec2 uv{(mouse.x - m_ViewportPos.x) / m_ViewportSize.x,
                        (mouse.y - m_ViewportPos.y) / m_ViewportSize.y};
                auto hit = m_Scene.Raycast(ViewportRay(uv));
                if (!hit) {
                    if (!io.KeyCtrl)
                        SelectOnly(0);
                } else {
                    // SketchUp convention: click selects the whole group,
                    // double-click drills to the exact part. Re-clicking inside
                    // an already-selected group also drills.
                    UUID pick = m_Scene.RootAncestor(hit->entity);
                    bool insideCurrent = m_Selected != 0 &&
                                         (m_Selected == pick ||
                                          m_Scene.IsDescendantOf(m_Selected, pick) ||
                                          m_Scene.RootAncestor(m_Selected) == pick);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || insideCurrent)
                        pick = hit->entity;
                    if (io.KeyCtrl)
                        ToggleSelection(pick);
                    else
                        SelectOnly(pick);
                }
            }

            // --- Marquee box select (drag that started on empty space) -----------
            if (!m_BoxSelecting && ImGui::IsItemHovered() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt &&
                !ImGuizmo::IsUsing() && !ImGuizmo::IsOver()) {
                ImVec2 mouse = ImGui::GetMousePos();
                vec2 uv{(mouse.x - m_ViewportPos.x) / m_ViewportSize.x,
                        (mouse.y - m_ViewportPos.y) / m_ViewportSize.y};
                if (!m_Scene.Raycast(ViewportRay(uv))) { // pressed on empty space
                    m_BoxSelecting = true;
                    m_BoxStartUV = uv;
                }
            }
            if (m_BoxSelecting) {
                // Same 5px threshold as click-to-select, so the two paths are
                // mutually exclusive: a tiny drag stays a click (deselect).
                bool dragged = io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] >= 25.0f;
                if (io.KeyAlt) {
                    m_BoxSelecting = false; // camera claimed the drag
                } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    if (dragged) {
                        ImVec2 a{m_ViewportPos.x + m_BoxStartUV.x * m_ViewportSize.x,
                                 m_ViewportPos.y + m_BoxStartUV.y * m_ViewportSize.y};
                        ImVec2 b = ImGui::GetMousePos();
                        ImVec2 mn{std::min(a.x, b.x), std::min(a.y, b.y)};
                        ImVec2 mx{std::max(a.x, b.x), std::max(a.y, b.y)};
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        dl->AddRectFilled(mn, mx, IM_COL32(86, 156, 214, 30));
                        dl->AddRect(mn, mx, IM_COL32(86, 156, 214, 200), 0.0f, 0, 1.5f);
                    }
                } else { // released
                    if (dragged) {
                        ImVec2 mouse = ImGui::GetMousePos();
                        vec2 endUV{(mouse.x - m_ViewportPos.x) / m_ViewportSize.x,
                                   (mouse.y - m_ViewportPos.y) / m_ViewportSize.y};
                        ApplyBoxSelect({glm::min(m_BoxStartUV, endUV), glm::max(m_BoxStartUV, endUV)},
                                       io.KeyCtrl);
                    }
                    m_BoxSelecting = false;
                }
            }
        }

        // --- Gizmo mode toolbar (top-left overlay; hidden while sculpting) ---
        if (!m_Sculpt.Active()) {
            ImGui::SetNextWindowPos(ImVec2(imgMin.x + 10, imgMin.y + 10));
            ImGui::SetNextWindowBgAlpha(0.70f);
            ImGui::Begin("##vp_toolbar", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking);
            auto modeBtn = [&](const char* label, GizmoOp op, const char* tip) {
                bool active = m_GizmoOp == op;
                if (active)
                    ui::PushAccentButton();
                if (ImGui::Button(label))
                    m_GizmoOp = op;
                if (active)
                    ui::PopAccentButton();
                ImGui::SetItemTooltip("%s", tip);
            };
            modeBtn("Move (W)", GizmoOp::Translate, "Drag the arrows to move");
            ImGui::SameLine();
            modeBtn("Rotate (E)", GizmoOp::Rotate, "Drag the rings to rotate");
            ImGui::SameLine();
            modeBtn("Scale (R)", GizmoOp::Scale, "Drag the handles to resize");
            ImGui::SameLine();
            // Latch the active state before drawing: the click below toggles
            // m_SnapEnabled, and Push/Pop must be balanced on the same value.
            bool snapActive = m_SnapEnabled;
            if (snapActive)
                ui::PushAccentButton();
            if (ImGui::Button("Snap")) {
                m_SnapEnabled = !m_SnapEnabled;
                FORGE_INFO("Snapping %s (grid %.2f, angle %.0f deg)", m_SnapEnabled ? "on" : "off",
                           m_SnapTranslate, m_SnapRotateDeg);
            }
            if (snapActive)
                ui::PopAccentButton();
            ImGui::SetItemTooltip("Snap moves/rotations to fixed steps.\nHold Ctrl to flip it for one drag");
            ImGui::End();
        }

        // --- Contextual status bar (bottom-left overlay, both modes) ---------
        ImGui::SetNextWindowPos(ImVec2(imgMin.x + 10, imgMin.y + avail.y - 34));
        ImGui::SetNextWindowBgAlpha(0.55f);
        ImGui::Begin("##vp_status", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoDocking |
                         ImGuiWindowFlags_NoInputs);
        ImGui::TextDisabled(m_Sculpt.Active()
                                ? "Drag to sculpt   Ctrl inverts   Shift smooths   Tab exits"
                            : m_Extrude.Busy()
                                ? "Press a flat face and drag to extrude   Esc cancels"
                                : "Alt+Drag orbit   MMB pan   Scroll zoom   F frame   Click select   "
                                  "Drag: box select   Ctrl+Click: add");
        ImGui::End();
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace forge
