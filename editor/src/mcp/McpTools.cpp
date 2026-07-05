#include "EditorApp.h"
#include "mcp/McpImage.h"
#include "mcp/McpScript.h"

#include <forge/assets/MeshFactory.h>
#include <forge/geometry/MeshBoolean.h>
#include <forge/geometry/MeshEdit.h>
#include <forge/geometry/MeshRemesh.h>
#include <forge/geometry/MeshStats.h>
#include <forge/renderer/Texture2D.h>

#include <json.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

// MCP tool surface (#76 perception, #77 actuation, #78 scripting). Registered
// handlers run on the GL main thread between frames (McpServer drains the
// queue there), so they may touch Scene/Renderer freely. Every mutating action
// goes through the CommandStack — agent ops sit in the editor's Ctrl+Z history
// like any other.

namespace forge {

using nlohmann::json;

// --- JSON helpers -------------------------------------------------------------
// Entity ids serialize as strings: a raw uint64 exceeds the 2^53 safe-integer
// range of the JS-centric MCP clients consuming this JSON.

static json Vec3Json(const vec3& v) { return {v.x, v.y, v.z}; }

// Reads [x,y,z] into out; true when the key is present and well-formed.
static bool GetVec3(const json& args, const char* key, vec3& out)
{
    if (!args.contains(key))
        return false;
    const json& a = args[key];
    if (!a.is_array() || a.size() != 3 || !a[0].is_number() || !a[1].is_number() ||
        !a[2].is_number())
        return false;
    out = {(float)a[0], (float)a[1], (float)a[2]};
    return true;
}

static json MaterialJson(const Material& m)
{
    json j;
    j["albedo"] = Vec3Json(m.albedo);
    j["metallic"] = m.metallic;
    j["roughness"] = m.roughness;
    if (m.emissiveStrength > 0.0f) {
        j["emissive"] = Vec3Json(m.emissive);
        j["emissiveStrength"] = m.emissiveStrength;
    }
    if (m.transmission > 0.0f) {
        j["transmission"] = m.transmission;
        j["ior"] = m.ior;
    }
    if (m.albedoMap)
        j["hasAlbedoMap"] = true;
    return j;
}

static json EntityJson(const Scene& scene, const Entity& e)
{
    json j;
    j["id"] = std::to_string(e.id);
    j["name"] = e.name;
    if (e.parent)
        j["parent"] = std::to_string(e.parent);
    j["translation"] = Vec3Json(e.transform.translation);
    j["rotationDeg"] = Vec3Json(glm::degrees(e.transform.rotation));
    j["scale"] = Vec3Json(e.transform.scale);
    j["worldPosition"] = Vec3Json(vec3(scene.WorldTransform(e.id)[3]));
    if (e.mesh)
        j["mesh"] = {{"vertices", e.mesh->Vertices().size()},
                     {"triangles", e.mesh->Indices().size() / 3}};
    j["material"] = MaterialJson(e.material);
    if (e.light.enabled)
        j["light"] = {{"color", Vec3Json(e.light.color)},
                      {"intensity", e.light.intensity},
                      {"range", e.light.range}};
    return j;
}

// Resolve a tool's target entity from {"id": "..."} (or {"name": "..."} as a
// fallback). Returns null + fills `error` when nothing matches. keyPrefix
// lets one call resolve secondary targets ({"otherId"/"otherName"}).
static Entity* FindToolTargetKeyed(Scene& scene, const json& args, const char* idKey,
                                   const char* nameKey, std::string& error)
{
    if (args.contains(idKey) && args[idKey].is_string()) {
        const std::string idStr = args[idKey];
        UUID id = 0;
        try {
            id = std::stoull(idStr);
        } catch (const std::exception&) {
            error = "Invalid entity id: " + idStr;
            return nullptr;
        }
        if (Entity* e = scene.Find(id))
            return e;
        error = "No entity with id " + idStr;
        return nullptr;
    }
    if (args.contains(nameKey) && args[nameKey].is_string()) {
        const std::string name = args[nameKey];
        for (Entity& e : scene.Entities())
            if (e.name == name)
                return &e;
        error = "No entity named \"" + name + "\"";
        return nullptr;
    }
    error = std::string("Provide \"") + idKey + "\" or \"" + nameKey + "\"";
    return nullptr;
}

static Entity* FindToolTarget(Scene& scene, const json& args, std::string& error)
{
    return FindToolTargetKeyed(scene, args, "id", "name", error);
}

static ToolResult JsonResult(const json& j)
{
    return ToolResult::Text(j.dump(2));
}

static ToolResult Err(std::string message)
{
    return ToolResult::Text(std::move(message), /*error=*/true);
}

// --- tool registration ----------------------------------------------------------

void EditorApp::RegisterMcpTools()
{
    m_McpProtocol.RegisterTool(
        "ping", "Health check for the Forge MCP server; returns \"pong\".",
        {{"type", "object"}, {"additionalProperties", false}},
        [](const json&) { return ToolResult::Text("pong"); });

    // --- perception (#76): read-only scene introspection ---------------------

    m_McpProtocol.RegisterTool(
        "get_scene",
        "Full scene snapshot: all entities (ids, names, hierarchy, transforms, material "
        "summaries, lights) plus sun, environment, and current selection.",
        {{"type", "object"}, {"additionalProperties", false}},
        [this](const json& args) { return ToolGetScene(args); });

    m_McpProtocol.RegisterTool(
        "get_entity", "Full detail for one entity, looked up by id (or name).",
        {{"type", "object"},
         {"properties",
          {{"id", {{"type", "string"}, {"description", "Entity id from get_scene"}}},
           {"name", {{"type", "string"}, {"description", "Entity name (first match)"}}}}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolGetEntity(args); });

    m_McpProtocol.RegisterTool(
        "get_mesh_stats",
        "Topology diagnostics for an entity's mesh: triangle/vertex counts, degenerate "
        "triangles, boundary and non-manifold edges, watertightness, UV presence, bounds.",
        {{"type", "object"},
         {"properties", {{"id", {{"type", "string"}}}, {"name", {{"type", "string"}}}}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolGetMeshStats(args); });

    m_McpProtocol.RegisterTool(
        "raycast",
        "Cast a ray and report the hit entity, position, and normal. Give either a world-"
        "space origin+direction, or u/v in [0,1] to pick through the viewport camera.",
        {{"type", "object"},
         {"properties",
          {{"origin", {{"type", "array"}, {"items", {{"type", "number"}}}, {"description", "world [x,y,z]"}}},
           {"direction", {{"type", "array"}, {"items", {{"type", "number"}}}, {"description", "world [x,y,z]"}}},
           {"u", {{"type", "number"}, {"description", "viewport x in [0,1]"}}},
           {"v", {{"type", "number"}, {"description", "viewport y in [0,1], top = 0"}}}}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolRaycast(args); });

    m_McpProtocol.RegisterTool(
        "viewport_screenshot",
        "Grab the current viewport image (what the user sees) as a PNG.",
        {{"type", "object"},
         {"properties",
          {{"maxDim", {{"type", "integer"}, {"description", "longest side in px, default 1024"}}}}},
         {"additionalProperties", false}},
        [this](const json& args) {
            int maxDim = std::clamp(args.value("maxDim", 1024), 64, 2048);
            const bool showRT = m_RayTracing && m_PathTracer.SampleCount() > 0;
            const uint32_t tex = showRT ? m_PathTracer.DisplayTexture() : m_DisplayTex;
            std::string b64 = TextureToPngBase64(tex, maxDim);
            if (b64.empty())
                return Err("No viewport image yet (editor still starting?)");
            ToolResult r;
            r.content = json::array(
                {{{"type", "image"}, {"data", std::move(b64)}, {"mimeType", "image/png"}}});
            return r;
        });

    // Async: dispatches ~8 spp per UI frame and answers when converged, so the
    // editor stays interactive during the render (see UpdateMcpRender).
    m_McpProtocol.RegisterToolAsync(
        "render_image",
        "Path-traced render of the scene from the current camera at the given resolution "
        "and samples-per-pixel. Runs in the background; the editor stays responsive.",
        {{"type", "object"},
         {"properties",
          {{"width", {{"type", "integer"}, {"description", "px, default 512, max 1024"}}},
           {"height", {{"type", "integer"}, {"description", "px, default 512, max 1024"}}},
           {"spp", {{"type", "integer"}, {"description", "samples per pixel, default 256, max 4096"}}}}},
         {"additionalProperties", false}},
        [this](const json& args, ToolResponder respond) {
            if (m_McpRender.active || m_Turntable.active) {
                respond(Err("A render is already in progress"));
                return;
            }
            const int w = std::clamp(args.value("width", 512), 64, 1024);
            const int h = std::clamp(args.value("height", 512), 64, 1024);

            m_PathTracer.Resize((uint32_t)w, (uint32_t)h);
            m_PathTracer.Upload(m_Scene);
            GatherLights();
            const mat4& view = m_Camera.View();
            m_PathTracer.SetLens(0.0f, 1.0f, vec3(view[0][0], view[1][0], view[2][0]),
                                 vec3(view[0][1], view[1][1], view[2][1]));
            m_PathTracer.SetDenoise(m_Denoise, m_DenoiseStrength);
            m_PathTracer.ResetAccumulation();

            m_McpRender.active = true;
            m_McpRender.sppTarget = std::clamp(args.value("spp", 256), 8, 4096);
            // Projection rebuilt for the requested aspect; the viewport's own
            // matrix would letterbox-stretch anything non-viewport-shaped.
            m_McpRender.viewProj = glm::perspective(glm::radians(m_Camera.FOV()),
                                                    (float)w / (float)h, 0.1f, 1000.0f) *
                                   view;
            m_McpRender.camPos = m_Camera.Position();
            m_McpRender.respond = std::move(respond);
        });

    // --- actuation (#77): every mutating action lands on the CommandStack ----

    m_McpProtocol.RegisterTool(
        "manage_entity",
        "Create and organize scene entities. Actions: spawn (primitive: cube|sphere|plane|"
        "cylinder|cone|torus|text, optional position/rotationDeg/scale/albedo/params), "
        "delete, duplicate, rename (newName), set_transform (any of position/rotationDeg/"
        "scale), set_parent (parentId/parentName, empty = root). Targets resolve by id or "
        "name. Returns the affected entity. All actions are undoable.",
        {{"type", "object"},
         {"properties",
          {{"action",
            {{"type", "string"},
             {"enum", {"spawn", "delete", "duplicate", "rename", "set_transform", "set_parent"}}}},
           {"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"primitive",
            {{"type", "string"},
             {"enum", {"cube", "sphere", "plane", "cylinder", "cone", "torus", "text"}}}},
           {"newName", {{"type", "string"}}},
           {"parentId", {{"type", "string"}, {"description", "empty string = unparent to root"}}},
           {"parentName", {{"type", "string"}}},
           {"position", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"rotationDeg", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"scale",
            {{"description", "uniform number or [x,y,z]"},
             {"type", {"array", "number"}}}},
           {"albedo", {{"type", "array"}, {"items", {{"type", "number"}}}, {"description", "[r,g,b] 0-1"}}},
           {"params",
            {{"type", "object"},
             {"description",
              "primitive detail: sphere {rings,sectors}; plane {size,subdivisions}; "
              "cylinder/cone {sectors}; torus {minorRadius,majorSectors,minorSectors}; "
              "text {text,depth,fontPath}"}}}}},
         {"required", {"action"}}},
        [this](const json& args) { return ToolManageEntity(args); });

    m_McpProtocol.RegisterTool(
        "manage_material",
        "Set material properties on an entity (any subset): albedo [r,g,b] 0-1, metallic, "
        "roughness, emissive [r,g,b], emissiveStrength, transmission (0=solid 1=glass), ior, "
        "albedoTexture (image file path; empty string clears). Undoable.",
        {{"type", "object"},
         {"properties",
          {{"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"albedo", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"metallic", {{"type", "number"}}},
           {"roughness", {{"type", "number"}}},
           {"emissive", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"emissiveStrength", {{"type", "number"}}},
           {"transmission", {{"type", "number"}}},
           {"ior", {{"type", "number"}}},
           {"albedoTexture", {{"type", "string"}}}}}},
        [this](const json& args) { return ToolManageMaterial(args); });

    m_McpProtocol.RegisterTool(
        "manage_light",
        "Lighting. Actions: spawn_point (position/color/intensity/range), set_point (entity "
        "target + enabled/color/intensity/range), set_sun (azimuthDeg/elevationDeg/intensity/"
        "color), set_environment (hdriPath to load an HDRI sky, intensity, rotationDeg). "
        "Point-light edits are undoable; sun/environment match the UI sliders (not undoable).",
        {{"type", "object"},
         {"properties",
          {{"action",
            {{"type", "string"},
             {"enum", {"spawn_point", "set_point", "set_sun", "set_environment"}}}},
           {"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"enabled", {{"type", "boolean"}}},
           {"position", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"color", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"intensity", {{"type", "number"}}},
           {"range", {{"type", "number"}}},
           {"azimuthDeg", {{"type", "number"}}},
           {"elevationDeg", {{"type", "number"}}},
           {"hdriPath", {{"type", "string"}}},
           {"rotationDeg", {{"type", "number"}}}}},
         {"required", {"action"}}},
        [this](const json& args) { return ToolManageLight(args); });

    m_McpProtocol.RegisterTool(
        "edit_mesh",
        "Semantic modeling ops on an entity's mesh. Actions: subdivide (keepShape: true = "
        "add resolution without smoothing), smooth (strength 0-1, iterations), boolean (op: "
        "union|subtract|intersect with otherId/otherName; replaces both operands), remesh "
        "(detail 32-160), mirror (bake X-mirror), extrude_face (pick a face via u/v or "
        "origin/direction ray on the target, push it out by distance). All undoable.",
        {{"type", "object"},
         {"properties",
          {{"action",
            {{"type", "string"},
             {"enum", {"subdivide", "smooth", "boolean", "remesh", "mirror", "extrude_face"}}}},
           {"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"keepShape", {{"type", "boolean"}}},
           {"strength", {{"type", "number"}}},
           {"iterations", {{"type", "integer"}}},
           {"op", {{"type", "string"}, {"enum", {"union", "subtract", "intersect"}}}},
           {"otherId", {{"type", "string"}}},
           {"otherName", {{"type", "string"}}},
           {"detail", {{"type", "integer"}}},
           {"u", {{"type", "number"}}},
           {"v", {{"type", "number"}}},
           {"origin", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"direction", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"distance", {{"type", "number"}}}}},
         {"required", {"action"}}},
        [this](const json& args) { return ToolEditMesh(args); });

    m_McpProtocol.RegisterTool(
        "manage_scene",
        "Scene-level ops. Actions: new (clears the scene, discards unsaved changes), open "
        "(path to .forge), save (optional path; required when untitled), import_model (path "
        "to .gltf/.glb/.obj), look_at (frame an entity by id/name or a point, optional "
        "distance), undo, redo.",
        {{"type", "object"},
         {"properties",
          {{"action",
            {{"type", "string"},
             {"enum", {"new", "open", "save", "import_model", "look_at", "undo", "redo"}}}},
           {"path", {{"type", "string"}}},
           {"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"point", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"distance", {{"type", "number"}}}}},
         {"required", {"action"}}},
        [this](const json& args) { return ToolManageScene(args); });

    // --- scripting (#78): code-as-actuation escape hatch ----------------------

    m_McpProtocol.RegisterTool(
        "execute_script",
        "Run a sandboxed Lua 5.4 script against the scene — loops, math, and "
        "variables for parametric builds that would take dozens of tool calls. "
        "The forge.* functions mirror the other tools and take one table of the "
        "same named fields (vectors as {x,y,z} arrays). Reads: scene(), "
        "get_entity{}, mesh_stats{}, raycast{}. Writes: spawn{}, delete{}, "
        "duplicate{}, rename{}, set_transform{}, set_parent{}, set_material{}, "
        "spawn_point_light{}, set_point_light{}, set_sun{}, set_environment{}, "
        "subdivide{}, smooth{}, boolean{}, remesh{}, mirror{}, extrude_face{}, "
        "look_at{}. Writes return the affected entity as a table (use .id). "
        "print() lines and the script's return value come back in the result. "
        "The whole script is ONE undo entry; on error the partial build rolls "
        "back (sun/environment/camera changes excepted). Sandboxed: no os/io/"
        "require; runaway loops and memory bombs abort.",
        {{"type", "object"},
         {"properties",
          {{"source", {{"type", "string"}, {"description", "Lua 5.4 source"}}}}},
         {"required", {"source"}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolExecuteScript(args); });
}

// --- perception handlers --------------------------------------------------------

ToolResult EditorApp::ToolGetScene(const json&)
{
    json j;
    j["entities"] = json::array();
    for (const Entity& e : m_Scene.Entities())
        j["entities"].push_back(EntityJson(m_Scene, e));
    j["sun"] = {{"azimuthDeg", m_SunAzimuth},
                {"elevationDeg", m_SunElevation},
                {"color", Vec3Json(m_Sun.color)},
                {"intensity", m_Sun.intensity}};
    if (m_Env && m_Env->Valid())
        j["environment"] = {{"path", m_EnvPath},
                            {"intensity", m_Env->intensity},
                            {"rotationDeg", m_Env->rotationDegrees}};
    json sel = json::array();
    for (UUID id : m_Selection)
        sel.push_back(std::to_string(id));
    j["selection"] = std::move(sel);
    return JsonResult(j);
}

ToolResult EditorApp::ToolGetEntity(const json& args)
{
    std::string error;
    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);
    json j = EntityJson(m_Scene, *e);
    if (e->mesh) {
        const AABB& b = e->mesh->Bounds();
        j["mesh"]["boundsMin"] = Vec3Json(b.min);
        j["mesh"]["boundsMax"] = Vec3Json(b.max);
    }
    json children = json::array();
    for (UUID id : m_Scene.ChildrenOf(e->id))
        children.push_back(std::to_string(id));
    j["children"] = std::move(children);
    return JsonResult(j);
}

ToolResult EditorApp::ToolGetMeshStats(const json& args)
{
    std::string error;
    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);
    if (!e->mesh)
        return Err("Entity \"" + e->name + "\" has no mesh");
    MeshStats s = ComputeMeshStats(e->mesh->Vertices(), e->mesh->Indices());
    json j{{"vertices", s.vertexCount},
           {"triangles", s.triangleCount},
           {"degenerateTriangles", s.degenerateTriangles},
           {"boundaryEdges", s.boundaryEdges},
           {"nonManifoldEdges", s.nonManifoldEdges},
           {"watertight", s.watertight},
           {"hasUVs", s.hasUVs},
           {"boundsMin", Vec3Json(s.bounds.min)},
           {"boundsMax", Vec3Json(s.bounds.max)},
           {"extents", Vec3Json(s.bounds.max - s.bounds.min)}};
    return JsonResult(j);
}

ToolResult EditorApp::ToolRaycast(const json& args)
{
    Ray ray;
    vec3 origin, direction;
    if (GetVec3(args, "origin", origin) && GetVec3(args, "direction", direction)) {
        ray.origin = origin;
        ray.direction = glm::normalize(direction);
    } else if (args.contains("u") && args.contains("v")) {
        ray = ViewportRay(vec2((float)args.value("u", 0.5), (float)args.value("v", 0.5)));
    } else {
        return Err("Provide origin+direction or u+v");
    }
    std::optional<RaycastHit> hit = m_Scene.Raycast(ray);
    if (!hit)
        return JsonResult(json{{"hit", false}});
    const Entity* e = m_Scene.Find(hit->entity);
    return JsonResult(json{{"hit", true},
                           {"entityId", std::to_string(hit->entity)},
                           {"entityName", e ? e->name : ""},
                           {"distance", hit->distance},
                           {"position", Vec3Json(hit->worldPos)},
                           {"normal", Vec3Json(hit->worldNormal)}});
}

// --- actuation handlers -------------------------------------------------------

ToolResult EditorApp::ToolManageEntity(const json& args)
{
    const std::string action = args.value("action", "");

    if (action == "spawn") {
        const std::string primitive = args.value("primitive", "");
        const json params = args.value("params", json::object());
        std::shared_ptr<Mesh> mesh;
        // Default-parameter spawns share the cached primitive meshes (they
        // serialize as recipes); custom parameters build a fresh mesh.
        if (primitive == "cube") {
            mesh = m_CubeMesh;
        } else if (primitive == "sphere") {
            mesh = params.empty() ? m_SphereMesh
                                  : MeshFactory::Sphere(params.value("rings", 32),
                                                        params.value("sectors", 48));
        } else if (primitive == "plane") {
            mesh = params.empty() ? m_PlaneMesh
                                  : MeshFactory::Plane(params.value("size", 1.0f),
                                                       params.value("subdivisions", 1));
        } else if (primitive == "cylinder") {
            mesh = params.empty() ? m_CylinderMesh
                                  : MeshFactory::Cylinder(params.value("sectors", 48));
        } else if (primitive == "cone") {
            mesh = params.empty() ? m_ConeMesh : MeshFactory::Cone(params.value("sectors", 48));
        } else if (primitive == "torus") {
            mesh = params.empty() ? m_TorusMesh
                                  : MeshFactory::Torus(params.value("minorRadius", 0.15f),
                                                       params.value("majorSectors", 48),
                                                       params.value("minorSectors", 24));
        } else if (primitive == "text") {
            const std::string text = params.value("text", "Forge");
            mesh = MeshFactory::Text(text, params.value("fontPath", "C:/Windows/Fonts/segoeui.ttf"),
                                     params.value("depth", 0.25f));
            if (!mesh)
                return Err("3D text failed (TTF fonts only)");
        } else {
            return Err("Unknown primitive: \"" + primitive + "\"");
        }

        std::string name = args.value("name", "");
        if (name.empty())
            name = primitive + " " + std::to_string(m_SpawnCounter);
        ++m_SpawnCounter;

        Entity& e = m_Scene.CreateEntity(name);
        e.mesh = mesh;
        e.transform.translation = {0.0f, 0.5f, 0.0f};
        GetVec3(args, "position", e.transform.translation);
        vec3 rotDeg{0.0f};
        if (GetVec3(args, "rotationDeg", rotDeg))
            e.transform.rotation = glm::radians(rotDeg);
        if (args.contains("scale")) {
            if (args["scale"].is_number())
                e.transform.scale = vec3((float)args["scale"]);
            else
                GetVec3(args, "scale", e.transform.scale);
        }
        GetVec3(args, "albedo", e.material.albedo);
        SelectOnly(e.id);
        m_Commands.Push(std::make_unique<AddEntityCommand>(e));
        return JsonResult(EntityJson(m_Scene, e));
    }

    std::string error;
    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);

    if (action == "delete") {
        const std::string deletedName = e->name;
        SelectOnly(e->id);
        DeleteSelected(); // composite subtree delete + undo entry
        return JsonResult(json{{"deleted", deletedName}});
    }
    if (action == "duplicate") {
        SelectOnly(e->id);
        DuplicateSelected();
        Entity* copy = m_Scene.Find(m_Selected);
        if (!copy)
            return Err("Duplicate failed");
        return JsonResult(EntityJson(m_Scene, *copy));
    }
    if (action == "rename") {
        if (!args.contains("newName") || !args["newName"].is_string())
            return Err("Provide newName");
        Entity before = *e;
        e->name = args["newName"];
        m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));
        return JsonResult(EntityJson(m_Scene, *e));
    }
    if (action == "set_transform") {
        Entity before = *e;
        bool any = GetVec3(args, "position", e->transform.translation);
        vec3 rotDeg;
        if (GetVec3(args, "rotationDeg", rotDeg)) {
            e->transform.rotation = glm::radians(rotDeg);
            any = true;
        }
        if (args.contains("scale")) {
            if (args["scale"].is_number()) {
                e->transform.scale = vec3((float)args["scale"]);
                any = true;
            } else {
                any |= GetVec3(args, "scale", e->transform.scale);
            }
        }
        if (!any)
            return Err("Provide at least one of position/rotationDeg/scale");
        m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));
        return JsonResult(EntityJson(m_Scene, *e));
    }
    if (action == "set_parent") {
        UUID parent = 0;
        const bool toRoot = args.value("parentId", "x") == std::string("") ||
                            args.value("parentName", "x") == std::string("");
        if (!toRoot) {
            std::string perr;
            Entity* p = FindToolTargetKeyed(m_Scene, args, "parentId", "parentName", perr);
            if (!p)
                return Err(perr);
            if (p->id == e->id || m_Scene.IsDescendantOf(p->id, e->id))
                return Err("Cannot parent an entity to itself or its descendant");
            parent = p->id;
        }
        Entity before = *e;
        m_Scene.ReparentKeepWorld(e->id, parent);
        m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));
        return JsonResult(EntityJson(m_Scene, *e));
    }
    return Err("Unknown action: \"" + action + "\"");
}

ToolResult EditorApp::ToolManageMaterial(const json& args)
{
    std::string error;
    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);

    Entity before = *e;
    Material& m = e->material;
    bool any = GetVec3(args, "albedo", m.albedo) | GetVec3(args, "emissive", m.emissive);
    auto num = [&](const char* key, float& out) {
        if (args.contains(key) && args[key].is_number()) {
            out = (float)args[key];
            any = true;
        }
    };
    num("metallic", m.metallic);
    num("roughness", m.roughness);
    num("emissiveStrength", m.emissiveStrength);
    num("transmission", m.transmission);
    num("ior", m.ior);
    if (args.contains("albedoTexture") && args["albedoTexture"].is_string()) {
        const std::string path = args["albedoTexture"];
        if (path.empty()) {
            m.albedoMap = nullptr;
        } else {
            auto tex = Texture2D::FromFile(path, /*srgb=*/true, /*flipV=*/false);
            if (!tex)
                return Err("Couldn't load texture: " + path);
            m.albedoMap = tex;
        }
        any = true;
    }
    if (!any)
        return Err("Provide at least one material property");
    m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));
    return JsonResult(EntityJson(m_Scene, *e));
}

ToolResult EditorApp::ToolManageLight(const json& args)
{
    const std::string action = args.value("action", "");

    if (action == "spawn_point") {
        Entity& e = m_Scene.CreateEntity("Point Light " + std::to_string(m_SpawnCounter++));
        e.mesh = m_SphereMesh;
        e.transform.translation = {0.0f, 2.0f, 0.0f};
        GetVec3(args, "position", e.transform.translation);
        e.transform.scale = vec3(0.2f);
        e.light.enabled = true;
        GetVec3(args, "color", e.light.color);
        if (args.contains("intensity") && args["intensity"].is_number())
            e.light.intensity = (float)args["intensity"];
        if (args.contains("range") && args["range"].is_number())
            e.light.range = (float)args["range"];
        e.material.albedo = e.light.color;
        e.material.emissive = e.light.color;
        e.material.emissiveStrength = 4.0f; // glows like the sidebar spawn
        SelectOnly(e.id);
        m_Commands.Push(std::make_unique<AddEntityCommand>(e));
        return JsonResult(EntityJson(m_Scene, e));
    }
    if (action == "set_point") {
        std::string error;
        Entity* e = FindToolTarget(m_Scene, args, error);
        if (!e)
            return Err(error);
        Entity before = *e;
        if (args.contains("enabled") && args["enabled"].is_boolean())
            e->light.enabled = args["enabled"];
        GetVec3(args, "color", e->light.color);
        if (args.contains("intensity") && args["intensity"].is_number())
            e->light.intensity = (float)args["intensity"];
        if (args.contains("range") && args["range"].is_number())
            e->light.range = (float)args["range"];
        m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));
        return JsonResult(EntityJson(m_Scene, *e));
    }
    if (action == "set_sun") {
        if (args.contains("azimuthDeg") && args["azimuthDeg"].is_number())
            m_SunAzimuth = (float)args["azimuthDeg"];
        if (args.contains("elevationDeg") && args["elevationDeg"].is_number())
            m_SunElevation = std::clamp((float)args["elevationDeg"], 5.0f, 89.0f);
        if (args.contains("intensity") && args["intensity"].is_number())
            m_Sun.intensity = (float)args["intensity"];
        GetVec3(args, "color", m_Sun.color);
        return JsonResult(json{{"azimuthDeg", m_SunAzimuth},
                               {"elevationDeg", m_SunElevation},
                               {"intensity", m_Sun.intensity},
                               {"color", Vec3Json(m_Sun.color)}});
    }
    if (action == "set_environment") {
        if (args.contains("hdriPath") && args["hdriPath"].is_string()) {
            const std::string path = args["hdriPath"];
            if (!LoadHDRIFile(path))
                return Err("Couldn't load HDRI: " + path);
        }
        if (!m_Env || !m_Env->Valid())
            return Err("No environment loaded; pass hdriPath first");
        if (args.contains("intensity") && args["intensity"].is_number())
            m_Env->intensity = (float)args["intensity"];
        if (args.contains("rotationDeg") && args["rotationDeg"].is_number())
            m_Env->rotationDegrees = (float)args["rotationDeg"];
        return JsonResult(json{{"path", m_EnvPath},
                               {"intensity", m_Env->intensity},
                               {"rotationDeg", m_Env->rotationDegrees}});
    }
    return Err("Unknown action: \"" + action + "\"");
}

ToolResult EditorApp::ToolEditMesh(const json& args)
{
    const std::string action = args.value("action", "");
    std::string error;
    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);
    if (!e->mesh)
        return Err("Entity \"" + e->name + "\" has no mesh");

    // Mesh-swap ops share one shape: compute the new mesh, swap, record.
    auto swapMesh = [&](std::shared_ptr<Mesh> after, const char* what) -> ToolResult {
        if (!after)
            return Err(std::string(what) + " failed");
        std::shared_ptr<Mesh> beforeMesh = e->mesh;
        e->mesh = after;
        m_Commands.Push(std::make_unique<MeshSwapCommand>(e->id, beforeMesh, after));
        return JsonResult(EntityJson(m_Scene, *e));
    };

    if (action == "subdivide")
        return swapMesh(LoopSubdivide(*e->mesh, args.value("keepShape", false)), "Subdivide");
    if (action == "smooth") {
        const float strength = std::clamp(args.value("strength", 0.5f), 0.0f, 1.0f);
        const int iterations = std::clamp(args.value("iterations", 2), 1, 20);
        return swapMesh(LaplacianSmoothMesh(*e->mesh, strength, iterations), "Smooth");
    }
    if (action == "remesh") {
        const int detail = std::clamp(args.value("detail", 64), 32, 160);
        return swapMesh(VoxelRemesh(*e->mesh, detail), "Remesh (empty or degenerate input?)");
    }
    if (action == "mirror")
        return swapMesh(MirrorBakeX(*e->mesh), "Mirror");

    if (action == "boolean") {
        std::string oerr;
        Entity* other = FindToolTargetKeyed(m_Scene, args, "otherId", "otherName", oerr);
        if (!other)
            return Err(oerr);
        if (!other->mesh)
            return Err("Entity \"" + other->name + "\" has no mesh");
        if (other->id == e->id)
            return Err("Boolean needs two different entities");
        const std::string opName = args.value("op", "union");
        BooleanOp op;
        if (opName == "union")
            op = BooleanOp::Union;
        else if (opName == "subtract")
            op = BooleanOp::Subtract;
        else if (opName == "intersect")
            op = BooleanOp::Intersect;
        else
            return Err("Unknown op: \"" + opName + "\"");

        // Mirror BooleanSelected: it consumes m_Selection[0] op m_Selection[1].
        SelectOnly(e->id);
        ToggleSelection(other->id);
        m_BoolStatus.clear();
        BooleanSelected(op);
        if (!m_BoolStatus.empty())
            return Err("Boolean failed: " + m_BoolStatus);
        Entity* result = m_Scene.Find(m_Selected);
        if (!result)
            return Err("Boolean produced no result");
        return JsonResult(EntityJson(m_Scene, *result));
    }

    if (action == "extrude_face") {
        if (!args.contains("distance") || !args["distance"].is_number())
            return Err("Provide distance (world units; negative pushes in)");
        Ray ray;
        vec3 origin, direction;
        if (GetVec3(args, "origin", origin) && GetVec3(args, "direction", direction)) {
            ray.origin = origin;
            ray.direction = glm::normalize(direction);
        } else if (args.contains("u") && args.contains("v")) {
            ray = ViewportRay(vec2((float)args.value("u", 0.5), (float)args.value("v", 0.5)));
        } else {
            return Err("Pick the face with u/v or origin/direction");
        }
        std::optional<RaycastHit> hit = m_Scene.Raycast(ray, e->id);
        if (!hit)
            return Err("Ray missed \"" + e->name + "\"");
        std::unique_ptr<Command> cmd =
            m_Extrude.ExtrudeHit(m_Scene, *hit, (float)args["distance"]);
        if (!cmd)
            return Err("Extrude failed");
        m_Commands.Push(std::move(cmd));
        return JsonResult(EntityJson(m_Scene, *e));
    }
    return Err("Unknown action: \"" + action + "\"");
}

ToolResult EditorApp::ToolManageScene(const json& args)
{
    const std::string action = args.value("action", "");

    if (action == "new") {
        DoNewScene(); // agent asked explicitly — skips the unsaved-changes modal
        return JsonResult(json{{"ok", true}, {"entities", 0}});
    }
    if (action == "open") {
        const std::string path = args.value("path", "");
        if (path.empty())
            return Err("Provide path");
        OpenSceneFile(path);
        if (m_ScenePath != path)
            return Err("Couldn't open " + path);
        return JsonResult(json{{"ok", true}, {"entities", m_Scene.Entities().size()}});
    }
    if (action == "save") {
        const std::string path = args.value("path", "");
        const std::string previousPath = m_ScenePath; // roll back on failure, like SaveSceneAs
        if (!path.empty())
            m_ScenePath = path;
        if (m_ScenePath.empty())
            return Err("Scene is untitled; provide path");
        if (!SaveScene()) {
            m_ScenePath = previousPath;
            return Err("Save failed: " + (path.empty() ? previousPath : path));
        }
        return JsonResult(json{{"ok", true}, {"path", m_ScenePath}});
    }
    if (action == "import_model") {
        const std::string path = args.value("path", "");
        if (path.empty())
            return Err("Provide path");
        if (!ImportModel(path))
            return Err("Couldn't import " + path);
        Entity* root = m_Scene.Find(m_Selected);
        return root ? JsonResult(EntityJson(m_Scene, *root))
                    : JsonResult(json{{"ok", true}});
    }
    if (action == "look_at") {
        vec3 point;
        float radius = args.value("distance", 4.0f);
        if (GetVec3(args, "point", point)) {
            m_Camera.Focus(point, radius);
        } else {
            std::string error;
            Entity* e = FindToolTarget(m_Scene, args, error);
            if (!e)
                return Err(error);
            const mat4 world = m_Scene.WorldTransform(e->id);
            point = vec3(world[3]);
            if (e->mesh && !args.contains("distance")) {
                const AABB& b = e->mesh->Bounds();
                const vec3 ext = (b.max - b.min) * e->transform.scale;
                radius = std::max({ext.x, ext.y, ext.z, 1.0f});
            }
            m_Camera.Focus(point, radius);
        }
        return JsonResult(json{{"ok", true}, {"focalPoint", Vec3Json(point)}});
    }
    if (action == "undo") {
        const bool ok = m_Commands.Undo(m_Scene);
        return JsonResult(json{{"ok", ok}, {"entities", m_Scene.Entities().size()}});
    }
    if (action == "redo") {
        const bool ok = m_Commands.Redo(m_Scene);
        return JsonResult(json{{"ok", ok}, {"entities", m_Scene.Entities().size()}});
    }
    return Err("Unknown action: \"" + action + "\"");
}

// --- scripting (#78) ------------------------------------------------------------

ToolResult EditorApp::ToolExecuteScript(const json& args)
{
    if (!args.contains("source") || !args["source"].is_string())
        return Err("Provide source (Lua)");
    const std::string source = args["source"];

    // Every command a binding pushes collects into one composite: the whole
    // script is a single undo entry, and a failed script rolls back atomically.
    m_Commands.BeginBatch();

    // Each forge.* function re-enters an existing tool handler with the same
    // JSON contract; an isError result becomes a Lua error (aborting the
    // script), a JSON result comes back as a table.
    auto call = [this](ToolResult (EditorApp::*handler)(const json&), const char* action) {
        return [this, handler, action](const json& fnArgs) -> json {
            json a = fnArgs;
            if (action)
                a["action"] = action;
            ToolResult r = (this->*handler)(a);
            std::string text;
            if (!r.content.empty() && r.content[0].contains("text") &&
                r.content[0]["text"].is_string())
                text = r.content[0]["text"];
            if (r.isError)
                throw std::runtime_error(text.empty() ? "tool failed" : text);
            json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
            return parsed.is_discarded() ? json(text) : parsed;
        };
    };

    ScriptResult run = RunSandboxedScript(source, [&](const ScriptInstall& add) {
        add("scene", call(&EditorApp::ToolGetScene, nullptr));
        add("get_entity", call(&EditorApp::ToolGetEntity, nullptr));
        add("mesh_stats", call(&EditorApp::ToolGetMeshStats, nullptr));
        add("raycast", call(&EditorApp::ToolRaycast, nullptr));

        add("spawn", call(&EditorApp::ToolManageEntity, "spawn"));
        add("delete", call(&EditorApp::ToolManageEntity, "delete"));
        add("duplicate", call(&EditorApp::ToolManageEntity, "duplicate"));
        add("rename", call(&EditorApp::ToolManageEntity, "rename"));
        add("set_transform", call(&EditorApp::ToolManageEntity, "set_transform"));
        add("set_parent", call(&EditorApp::ToolManageEntity, "set_parent"));

        add("set_material", call(&EditorApp::ToolManageMaterial, nullptr));
        add("spawn_point_light", call(&EditorApp::ToolManageLight, "spawn_point"));
        add("set_point_light", call(&EditorApp::ToolManageLight, "set_point"));
        add("set_sun", call(&EditorApp::ToolManageLight, "set_sun"));
        add("set_environment", call(&EditorApp::ToolManageLight, "set_environment"));

        add("subdivide", call(&EditorApp::ToolEditMesh, "subdivide"));
        add("smooth", call(&EditorApp::ToolEditMesh, "smooth"));
        add("boolean", call(&EditorApp::ToolEditMesh, "boolean"));
        add("remesh", call(&EditorApp::ToolEditMesh, "remesh"));
        add("mirror", call(&EditorApp::ToolEditMesh, "mirror"));
        add("extrude_face", call(&EditorApp::ToolEditMesh, "extrude_face"));

        add("look_at", call(&EditorApp::ToolManageScene, "look_at"));
    });

    std::unique_ptr<CompositeCommand> batch = m_Commands.EndBatch();
    if (!run.ok) {
        if (batch && !batch->Empty())
            batch->Undo(m_Scene); // roll back the partial build
        if (!m_Scene.Find(m_Selected)) {
            m_Selection.clear();
            m_Selected = 0;
        }
        std::string msg = run.error;
        if (!run.output.empty())
            msg += "\n--- script output ---\n" + run.output;
        return Err(std::move(msg));
    }
    if (batch && !batch->Empty())
        m_Commands.Push(std::move(batch));
    if (!m_Scene.Find(m_Selected)) { // script may end on a delete
        m_Selection.clear();
        m_Selected = 0;
    }

    json out{{"ok", true}, {"entities", m_Scene.Entities().size()}};
    if (!run.output.empty())
        out["output"] = run.output;
    if (!run.returned.is_null())
        out["returned"] = run.returned;
    return JsonResult(out);
}

void EditorApp::UpdateMcpRender()
{
    McpRenderJob& job = m_McpRender;
    if (!job.active)
        return;

    m_PathTracer.Dispatch(job.viewProj, job.camPos, m_Sun, m_Bounces, m_FrameLights,
                          m_Env.get(), 8);
    if (m_PathTracer.SampleCount() < job.sppTarget)
        return;

    ToolResult r;
    std::string b64 = TextureToPngBase64(m_PathTracer.DisplayTexture(), 1024);
    if (b64.empty()) {
        r = Err("Render readback failed");
    } else {
        r.content = json::array(
            {{{"type", "image"}, {"data", std::move(b64)}, {"mimeType", "image/png"}},
             {{"type", "text"},
              {"text", std::to_string(m_PathTracer.Width()) + "x" +
                           std::to_string(m_PathTracer.Height()) + " render, " +
                           std::to_string(m_PathTracer.SampleCount()) + " spp"}}});
    }
    job.active = false;
    ToolResponder respond = std::move(job.respond);
    job.respond = nullptr;
    m_LastSceneHash = 0; // interactive RT must re-upload + resize after we hijacked the PT
    respond(std::move(r));
}

} // namespace forge
