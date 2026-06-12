#include "EditorApp.h"
#include "FileDialog.h"
#include "Theme.h"

#include <forge/assets/AssetManager.h>
#include <forge/assets/MeshFactory.h>
#include <forge/core/Log.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <ImGuizmo.h>

#include <cmath>
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
}

EditorApp::~EditorApp()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void EditorApp::Run()
{
    while (!m_Window.ShouldClose()) {
        m_Window.PollEvents();

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
        bool rtActive = m_RayTracing && !m_Sculpt.Active();
        if (rtActive)
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
    if (Entity* sel = m_Scene.Find(m_Selected); sel && sel->mesh && !m_Sculpt.Active())
        m_Renderer.SetOutline(*sel->mesh, m_Scene.WorldTransform(sel->id));
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

    mat4 viewProj = m_Camera.ViewProjection();
    bool reset = viewProj != m_LastViewProj || std::memcmp(&m_Sun, &m_LastSun, sizeof(m_Sun)) != 0 ||
                 m_Bounces != m_LastBounces || envChanged;
    if (reset)
        m_PathTracer.ResetAccumulation();
    m_LastViewProj = viewProj;
    m_LastSun = m_Sun;
    m_LastBounces = m_Bounces;

    // 1 spp while interacting (latency), 4 spp while converging (4x faster).
    m_PathTracer.Dispatch(viewProj, m_Camera.Position(), m_Sun, m_Bounces, m_FrameLights, m_Env.get(),
                          reset ? 1 : 4);
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

    if (ImGui::IsKeyPressed(ImGuiKey_Delete))
        DeleteSelected();
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
    return true;
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
            if (ImGui::Button("Clear HDRI", ImVec2(-1, 0)))
                m_Env.reset();
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
            ImGui::SetItemTooltip("Light bounces. 3-4 is usually enough;\nmore adds noise faster than realism");
            bool ground = m_PathTracer.GroundPlane();
            if (ImGui::Checkbox("Ground plane", &ground))
                m_PathTracer.SetGroundPlane(ground);
            ImGui::SetItemTooltip("A studio floor that catches shadows");
            ImGui::TextDisabled("%d samples, %zu triangles", m_PathTracer.SampleCount(),
                                m_PathTracer.TriangleCount());
        }
    }

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
    auto vec3Row = [&](const char* label, vec3& v, float speed, float resetVal, bool* changed) {
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
            if (ImGui::DragFloat("##v", &(&v.x)[a], speed, 0.0f, 0.0f, "%.2f") && changed)
                *changed = true;
            track();
            ImGui::PopID();
        }
        ImGui::PopID();
    };

    vec3Row("Position", e->transform.translation, 0.05f, 0.0f, nullptr);
    vec3 deg = glm::degrees(e->transform.rotation);
    bool rotChanged = false;
    vec3Row("Rotation", deg, 0.5f, 0.0f, &rotChanged);
    if (rotChanged)
        e->transform.rotation = glm::radians(deg);
    vec3Row("Scale", e->transform.scale, 0.02f, 1.0f, nullptr);

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
        bool showRT = m_RayTracing && !m_Sculpt.Active() && !m_RTUploadPending;
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
        } else {
            // --- Gizmo (manipulates the primary selection through its parent chain) ---
            Entity* sel = m_Scene.Find(m_Selected);
            if (sel) {
                ImGuizmo::SetOrthographic(false);
                ImGuizmo::SetDrawlist();
                ImGuizmo::SetRect(m_ViewportPos.x, m_ViewportPos.y, m_ViewportSize.x, m_ViewportSize.y);

                ImGuizmo::OPERATION op = m_GizmoOp == GizmoOp::Translate ? ImGuizmo::TRANSLATE
                                       : m_GizmoOp == GizmoOp::Rotate    ? ImGuizmo::ROTATE
                                                                         : ImGuizmo::SCALE;
                mat4 parentWorld = m_Scene.WorldTransform(sel->parent);
                mat4 model = parentWorld * sel->transform.World();
                ImGuizmo::Manipulate(glm::value_ptr(m_Camera.View()), glm::value_ptr(m_Camera.Projection()),
                                     op, ImGuizmo::LOCAL, glm::value_ptr(model));

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
                                : "Alt+Drag orbit   MMB pan   Scroll zoom   F frame   Click select");
        ImGui::End();
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace forge
