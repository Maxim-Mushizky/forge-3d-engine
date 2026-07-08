#include "EditorApp.h"
#include "mcp/McpElements.h"
#include "mcp/McpImage.h"
#include "mcp/McpScript.h"
#include "mcp/McpSculpt.h"
#include "mcp/McpViews.h"

#include <forge/anim/Pose.h>
#include <forge/anim/PosePresets.h>
#include <forge/anim/Skeleton.h>
#include <forge/anim/SkinApply.h>
#include <forge/assets/MeshFactory.h>
#include <forge/assets/StlExporter.h>
#include <forge/geometry/EditMesh.h>
#include <forge/geometry/MeshBoolean.h>
#include <forge/geometry/MeshEdit.h>
#include <forge/geometry/MeshRemesh.h>
#include <forge/geometry/MeshStats.h>
#include <forge/geometry/Placement.h>
#include <forge/geometry/Silhouette.h>
#include <forge/geometry/Spatial.h>
#include <forge/geometry/UvUnwrap.h>
#include <forge/renderer/Texture2D.h>
#include <forge/renderer/TextureGen.h>
#include <forge/renderer/TextureSource.h>
#include <forge/scene/DropToGround.h>

#include <json.hpp>
#include <stb_image.h> // reference decode for compare_silhouette (#114)

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

// Reads an [x,y,z] json value into out; true when well-formed.
static bool Vec3FromJson(const json& a, vec3& out)
{
    if (!a.is_array() || a.size() != 3 || !a[0].is_number() || !a[1].is_number() ||
        !a[2].is_number())
        return false;
    // NaN/Inf and float-overflowing doubles pass is_number(); treat them as
    // malformed and leave `out` untouched. Range-check before the float cast
    // — casting an out-of-range double is itself UB. Note the false return
    // makes a poisoned vec3 indistinguishable from an absent key at optional
    // call sites; acceptable only because both entry paths (dispatch and the
    // script binding wrapper) already reject non-finite args up front (#104).
    const double x = a[0].get<double>(), y = a[1].get<double>(), z = a[2].get<double>();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        std::fabs(x) > (double)FLT_MAX || std::fabs(y) > (double)FLT_MAX ||
        std::fabs(z) > (double)FLT_MAX)
        return false;
    out = {(float)x, (float)y, (float)z};
    return true;
}

// Reads [x,y,z] into out; true when the key is present and well-formed.
static bool GetVec3(const json& args, const char* key, vec3& out)
{
    return args.contains(key) && Vec3FromJson(args[key], out);
}

// Reads [x,y,z,w] into out (glm order is w,x,y,z); true when present + well-formed.
// Finiteness is already guaranteed by the front-door NaN/Inf guard, but the array
// shape and a non-degenerate magnitude still need checking here.
static bool GetQuat(const json& args, const char* key, glm::quat& out)
{
    if (!args.contains(key))
        return false;
    const json& a = args[key];
    if (!a.is_array() || a.size() != 4 || !a[0].is_number() || !a[1].is_number() ||
        !a[2].is_number() || !a[3].is_number())
        return false;
    glm::quat q((float)a[3].get<double>(), (float)a[0].get<double>(),
                (float)a[1].get<double>(), (float)a[2].get<double>()); // (w, x, y, z)
    if (glm::length(q) < 1e-6f)
        return false;
    out = glm::normalize(q);
    return true;
}

// Applies optional orbit-pose args (focalPoint/distance/pitchDeg/yawDeg/
// fovDeg) to a camera; each field only when present. Shared by set_camera and
// render_image (#138) so both speak the same parameterization.
static void ApplyPoseArgs(const json& args, EditorCamera& cam)
{
    EditorCamera::Bookmark b = cam.GetBookmark();
    GetVec3(args, "focalPoint", b.focalPoint);
    if (args.contains("distance") && args["distance"].is_number())
        b.distance = std::max(0.05f, (float)args["distance"]);
    if (args.contains("pitchDeg") && args["pitchDeg"].is_number())
        b.pitch = glm::radians(std::clamp((float)args["pitchDeg"], -88.0f, 88.0f));
    if (args.contains("yawDeg") && args["yawDeg"].is_number())
        b.yaw = glm::radians((float)args["yawDeg"]);
    cam.ApplyBookmark(b);
    if (args.contains("fovDeg") && args["fovDeg"].is_number())
        cam.SetFOV((float)args["fovDeg"]);
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
    if (m.subsurface > 0.0f) {
        j["subsurface"] = m.subsurface;
        j["subsurfaceColor"] = Vec3Json(m.subsurfaceColor);
        j["subsurfaceRadius"] = Vec3Json(m.subsurfaceRadius);
    }
    if (m.albedoMap)
        j["hasAlbedoMap"] = true;
    if (m.metallicRoughnessMap)
        j["hasMRMap"] = true;
    // Rebuildable descriptors ("file:..."/"proc:...", #113) so an agent can
    // read back what it assigned; absent for sourceless (glTF-embedded) maps.
    if (!m.albedoSource.empty())
        j["albedoSource"] = m.albedoSource;
    if (!m.mrSource.empty())
        j["mrSource"] = m.mrSource;
    return j;
}

// World-space AABB of an entity's mesh; invalid AABB for meshless nodes.
static AABB WorldBoundsOf(const Scene& scene, const Entity& e)
{
    if (!e.mesh)
        return AABB{};
    return TransformAABB(e.mesh->Bounds(), scene.WorldTransform(e.id));
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
    if (e.mesh) {
        j["mesh"] = {{"vertices", e.mesh->Vertices().size()},
                     {"triangles", e.mesh->Indices().size() / 3}};
        if (!e.mesh->Submeshes().empty())
            j["mesh"]["submeshes"] = e.mesh->Submeshes().size();
        const AABB wb = WorldBoundsOf(scene, e);
        if (wb.Valid())
            j["worldBounds"] = {{"min", Vec3Json(wb.min)},
                                {"max", Vec3Json(wb.max)},
                                {"center", Vec3Json((wb.min + wb.max) * 0.5f)},
                                {"extents", Vec3Json(wb.max - wb.min)}};
    }
    j["material"] = MaterialJson(e.material);
    if (!e.extraMaterials.empty()) {
        j["materialSlots"] = MaterialSlotCount(e);
        json mats = json::array();
        for (uint32_t s = 0; s < MaterialSlotCount(e); ++s)
            mats.push_back(MaterialJson(MaterialForSlot(e, s)));
        j["materials"] = std::move(mats); // per-slot materials (#80); "material" stays slot 0
    }
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

// forge.measure's `entity` accepts an id (number or numeric string) or a name.
// Digit-only strings try the id first, then fall back to a name scan — an
// entity literally named "42" still resolves.
static Entity* FindEntityFlexible(Scene& scene, const json& v, std::string& error)
{
    std::string text;
    if (v.is_number_unsigned())
        text = std::to_string(v.get<uint64_t>());
    else if (v.is_number_integer())
        text = std::to_string(v.get<int64_t>());
    else if (v.is_string())
        text = v.get<std::string>();
    else {
        error = "entity must be an id or a name";
        return nullptr;
    }
    if (!text.empty() &&
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); })) {
        try {
            if (Entity* e = scene.Find(std::stoull(text)))
                return e;
        } catch (const std::exception&) {
            // out-of-range digits: fall through to the name scan
        }
    }
    for (Entity& e : scene.Entities())
        if (e.name == text)
            return &e;
    error = "no entity matching \"" + text + "\"";
    return nullptr;
}

// Whole-scene framing bounds (#93). A ground plane dominates the raw union
// and shrinks everything else to dots, so essentially-flat meshes are ignored
// when anything with volume exists.
static AABB SceneFocusBounds(Scene& scene)
{
    AABB all, solid;
    for (const Entity& e : scene.Entities())
        if (e.mesh) {
            const AABB b = WorldBoundsOf(scene, e);
            all.Expand(b.min);
            all.Expand(b.max);
            const vec3 ext = b.max - b.min;
            if (ext.y > 0.02f * std::max(ext.x, ext.z)) {
                solid.Expand(b.min);
                solid.Expand(b.max);
            }
        }
    return solid.Valid() ? solid : all;
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
        "Full scene snapshot: all entities (ids, names, hierarchy, transforms, world-"
        "space AABBs, material summaries, lights) plus sun, environment, and current "
        "selection.",
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
        "triangles, boundary and non-manifold edges, watertightness, UV presence, UV area "
        "coverage (~atlas utilization; >1 = overlapping charts), bounds.",
        {{"type", "object"},
         {"properties", {{"id", {{"type", "string"}}}, {"name", {{"type", "string"}}}}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolGetMeshStats(args); });

    m_McpProtocol.RegisterTool(
        "measure",
        "World-space measurement (#114) — proportions as numbers, not eyeballs. Two "
        "forms: distance between endpoints a/b (each an [x,y,z] literal or {entity, "
        "feature} with feature top|bottom|center, landmarks on the subtree's world "
        "AABB), or extents of one entity (entity/id/name, optional axis x|y|z). Use "
        "after every build: a cup whose height/diameter ratio is off reads wrong "
        "long before a render shows it.",
        {{"type", "object"},
         {"properties",
          {{"a", {{"description", "[x,y,z] or {entity, feature: top|bottom|center}"}}},
           {"b", {{"description", "[x,y,z] or {entity, feature: top|bottom|center}"}}},
           {"entity", {{"description", "extents form: entity id or name"}}},
           {"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"axis",
            {{"type", "string"},
             {"enum", {"x", "y", "z"}},
             {"description", "extents form: report one axis instead of all three"}}}}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolMeasure(args); });

    m_McpProtocol.RegisterTool(
        "compare_silhouette",
        "Shape verification against a reference image (#114): software-rasterizes "
        "the entity subtree (or whole scene) into a binary orthographic silhouette, "
        "binarizes the reference (alpha matte when present, else border flood with "
        "Otsu fallback; enclosed through-holes in opaque references — backdrop seen "
        "through cutouts — are recovered automatically, #134), tight-crops "
        "and uniformly rescales both, and returns IoU/Dice plus pass vs threshold, "
        "the silhouette PNG, and a diff PNG (gray = match, green = render only, "
        "magenta = reference only). IoU >= ~0.9 = matching shape; proportion errors "
        "show as one-sided green/magenta bands. Its ingest counterpart is "
        "analyze_reference (#135), which turns a reference image into a simplified "
        "outline + landmarks to model FROM. Structured diff (#136): regions[] lists "
        "the mismatch as data — 4-connected components over the diff classes, "
        "largest first (capped, speck-filtered, remainder in regionSummary). type "
        "'missing' = the reference has material there: grow or move something "
        "toward centroidWorld; 'excess' = the model overshoots: shrink/trim there. "
        "centroidPx/bboxPx index the returned normalized silhouette image (the "
        "size x size compare frame), NOT the reference image — unlike "
        "analyze_reference, whose Px fields are source-image coordinates. "
        "centroidWorld/bboxWorld are in the target's world frame on the view's "
        "depth plane, so fixes are computable without reading any image. Frame "
        "caveat: both masks are normalized (crop+center+scale), so a defect that "
        "changes the silhouette's OUTER BBOX re-centers the frame and smears a "
        "one-sided error into a symmetric half-magnitude pair — prefer fixes "
        "anchored to bbox-preserving datums, or iterate (the loop still "
        "converges; see docs/mcp/examples/closed-loop-fix.lua). "
        "symmetry.render/.reference score each silhouette against its own "
        "left-right mirror (1.0 = perfectly bilateral): reference high + render "
        "low = the build broke a symmetry the subject has. "
        "Multi-view (#137): references=[{path,view}] runs the compare once per "
        "entry against the same target (2-4 photos = real replica gating); "
        "result = combined{iou=min across views — the honest gate, meanIou, "
        "pass=all views pass} + views[] each carrying the full single-view "
        "block incl. regions[]. Single-reference form unchanged. Views are the "
        "axis-aligned set only; matching an arbitrary-yaw photo needs camera "
        "calibration against it — future work.",
        {{"type", "object"},
         {"properties",
          {{"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"view",
            {{"type", "string"},
             {"enum", {"front", "back", "left", "right", "top"}},
             {"description", "orthographic view axis, default front"}}},
           {"reference",
            {{"type", "string"}, {"description", "path to the reference image (png/jpg)"}}},
           {"references",
            {{"type", "array"},
             {"minItems", 1},
             {"maxItems", 8},
             {"description",
              "multi-view form (#137): one compare per entry; mutually "
              "exclusive with reference/view"},
             {"items",
              {{"type", "object"},
               {"properties",
                {{"path",
                  {{"type", "string"},
                   {"description", "path to the reference image (png/jpg)"}}},
                 {"view",
                  {{"type", "string"}, {"enum", {"front", "back", "left", "right", "top"}}}}}},
               {"required", {"path", "view"}},
               {"additionalProperties", false}}}}},
           {"threshold",
            {{"type", "number"},
             {"description", "pass when IoU >= this (every view, and combined), default 0.8"}}},
           {"size",
            {{"type", "integer"}, {"description", "mask resolution, 64-1024, default 256"}}}}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolCompareSilhouette(args); });

    m_McpProtocol.RegisterTool(
        "analyze_reference",
        "Ingest half of the shape loop (compare_silhouette is the iterate half, "
        "#135): binarize a reference image, trace its silhouette to a simplified "
        "polygon outline (with holes, islands nested inside them, and any extra "
        "outlines), and report landmarks "
        "— bbox, centroid, widest row / tallest column, extremities — plus optional "
        "per-row spans. Everything is reported in bbox-centered model space (x "
        "right, y UP, the larger bbox side spanning 1.0 unit), ready to feed sweep "
        "sections and lathe profiles. Re-rasterizes the simplified polygons and "
        "self-reports IoU vs the source mask. mirror=\"x\" adds a half-profile "
        "(r,y) folded about the vertical axis, plus a lathe-ready profile when its "
        "ends leave the axis. Numbers only — no images.",
        {{"type", "object"},
         {"properties",
          {{"image",
            {{"type", "string"}, {"description", "path to the reference image (png/jpg)"}}},
           {"maxPoints",
            {{"type", "integer"},
             {"description",
              "outline point budget, 8-512, default 64; holes and extra outlines "
              "use min(this, 32)"}}},
           {"tolerancePx",
            {{"type", "number"},
             {"description", "simplification tolerance in px, 0-50, default 1.0"}}},
           {"mirror",
            {{"type", "string"},
             {"enum", {"x"}},
             {"description", "\"x\" folds a half-profile about the vertical axis"}}},
           {"spanRows",
            {{"type", "integer"},
             {"description", "sample this many evenly-spaced per-row spans, 0-4096, "
                             "default 0 = none"}}}}},
         {"required", {"image"}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolAnalyzeReference(args); });

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
        "check_overlap",
        "AABB overlap test between two entities (#94): overlap bool, penetration "
        "vector (translate the first entity by it to separate), depth, and gap "
        "distance when clear. Face contact counts as not overlapping. Use this to "
        "detect interpenetrating parts numerically instead of squinting at pixels.",
        {{"type", "object"},
         {"properties",
          {{"id", {{"type", "string"}, {"description", "first entity id"}}},
           {"name", {{"type", "string"}, {"description", "first entity name"}}},
           {"otherId", {{"type", "string"}, {"description", "second entity id"}}},
           {"otherName", {{"type", "string"}, {"description", "second entity name"}}}}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolCheckOverlap(args); });

    m_McpProtocol.RegisterTool(
        "query_spatial",
        "Spatial queries over the scene (#94). Actions: within_radius (entities whose "
        "bounds lie within radius of a point [point-to-bounds distance] or of another "
        "entity's bounds [bounds-to-bounds distance; the center entity is excluded]; "
        "point wins when both are given; sorted by distance), "
        "above_height / below_height (entities entirely above/below a world y), "
        "ground_height (top surface height at x,z via downward raycast — use before "
        "placing something).",
        {{"type", "object"},
         {"properties",
          {{"action",
            {{"type", "string"},
             {"enum", {"within_radius", "above_height", "below_height", "ground_height"}}}},
           {"point", {{"type", "array"}, {"items", {{"type", "number"}}},
                      {"description", "world [x,y,z] center for within_radius"}}},
           {"id", {{"type", "string"}, {"description", "center entity for within_radius"}}},
           {"name", {{"type", "string"}}},
           {"radius", {{"type", "number"}}},
           {"height", {{"type", "number"}, {"description", "world y for above/below"}}},
           {"x", {{"type", "number"}, {"description", "ground_height sample x"}}},
           {"z", {{"type", "number"}, {"description", "ground_height sample z"}}}}},
         {"required", {"action"}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolQuerySpatial(args); });

    // --- relational placement (#95): declarative relations, engine-solved ------

    m_McpProtocol.RegisterTool(
        "place_relative",
        "Place an entity by relation to an anchor — the engine solves the transform "
        "from world AABBs, so don't compute coordinates. Relations: on (rest on the "
        "anchor's top; keeps the entity's XZ when already over the footprint), above "
        "(same but floating, clearance default 0.25), against (abut a side: +x/-x/+z/-z "
        "or nearest; bottoms aligned), facing (yaw toward the anchor, no move), around "
        "(ring of count copies — or of the listed ids/names — bottoms on the anchor's "
        "floor, each facing it). on/above/against auto-nudge out of collisions with "
        "third parties and report residualOverlap when a nudge couldn't fully resolve; "
        "around reports residualOverlap when the ring is too crowded for the count. "
        "One undo entry.",
        {{"type", "object"},
         {"properties",
          {{"id", {{"type", "string"}, {"description", "entity to place"}}},
           {"name", {{"type", "string"}}},
           {"otherId", {{"type", "string"}, {"description", "anchor entity"}}},
           {"otherName", {{"type", "string"}}},
           {"relation",
            {{"type", "string"}, {"enum", {"on", "above", "against", "facing", "around"}}}},
           {"clearance", {{"type", "number"}, {"description", "gap in world units"}}},
           {"side",
            {{"type", "string"}, {"enum", {"+x", "-x", "+z", "-z"}},
             {"description", "against only; default nearest"}}},
           {"count", {{"type", "integer"}, {"description", "around only: copies of the entity"}}},
           {"ids", {{"type", "array"}, {"items", {{"type", "string"}}},
                    {"description", "around only: arrange these existing entities instead"}}},
           {"names", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
         {"required", {"relation"}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolPlaceRelative(args); });

    m_McpProtocol.RegisterTool(
        "snap_to_surface",
        "Rest an entity (with children) on the nearest surface: straight down by "
        "default (AABB drop onto whatever is underneath, ground plane at y=0 as "
        "fallback), or cast along an explicit direction and land the leading face on "
        "the first surface hit. Undoable.",
        {{"type", "object"},
         {"properties",
          {{"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"direction", {{"type", "array"}, {"items", {{"type", "number"}}},
                          {"description", "world [x,y,z]; omit for straight down"}}},
           {"clearance", {{"type", "number"}, {"description", "gap from the surface, default 0"}}}}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolSnapToSurface(args); });

    m_McpProtocol.RegisterTool(
        "align_entities",
        "Align several entities on one axis: bring each one's min face / center / max "
        "face (mode, default center) to the first entity's — or to an explicit target "
        "coordinate. One undo entry.",
        {{"type", "object"},
         {"properties",
          {{"ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
           {"names", {{"type", "array"}, {"items", {{"type", "string"}}}}},
           {"axis", {{"type", "string"}, {"enum", {"x", "y", "z"}}}},
           {"mode", {{"type", "string"}, {"enum", {"min", "center", "max"}}}},
           {"target", {{"type", "number"}, {"description", "world coordinate; default = first entity's"}}}}},
         {"required", {"axis"}},
         {"additionalProperties", false}},
        [this](const json& args) {
            json a = args;
            a["action"] = "align";
            return ToolArrangeEntities(a);
        });

    m_McpProtocol.RegisterTool(
        "distribute_entities",
        "Space several entities along one axis, keeping their current spatial order: "
        "with spacing, packs them gap-to-gap starting from the box furthest toward "
        "-axis (which stays put); without, spreads centers evenly between the current "
        "outermost two. Listing order does not matter. One undo entry.",
        {{"type", "object"},
         {"properties",
          {{"ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
           {"names", {{"type", "array"}, {"items", {{"type", "string"}}}}},
           {"axis", {{"type", "string"}, {"enum", {"x", "y", "z"}}}},
           {"spacing", {{"type", "number"}, {"description", "gap between bounds; omit to spread evenly"}}}}},
         {"required", {"axis"}},
         {"additionalProperties", false}},
        [this](const json& args) {
            json a = args;
            a["action"] = "distribute";
            return ToolArrangeEntities(a);
        });

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
        "Path-traced render of the scene at the given resolution and samples-per-pixel. "
        "Camera: the live editor camera by default, or an explicit hermetic pose via "
        "focalPoint/distance/pitchDeg/yawDeg/fovDeg (same orbit parameterization as "
        "set_camera; #138) — pass the pose to make renders reproducible even if the "
        "viewport camera moves between calls. Runs in the background; the editor stays "
        "responsive. Optional per-render quality overrides (#92): bounces 1-16 (raise to "
        "8+ for glass interiors), denoise + denoiseStrength 0-1, and thin-lens depth of "
        "field via aperture (world units, 0 = pinhole) + focusDist (distance to the sharp "
        "plane; omit to focus on the orbit point). Overrides apply to this render only "
        "- the editor's interactive settings are untouched.",
        {{"type", "object"},
         {"properties",
          {{"width", {{"type", "integer"}, {"description", "px, default 512, max 1024"}}},
           {"height", {{"type", "integer"}, {"description", "px, default 512, max 1024"}}},
           {"spp", {{"type", "integer"}, {"description", "samples per pixel, default 256, max 4096"}}},
           {"bounces", {{"type", "integer"}, {"description", "path depth 1-16, default = editor setting"}}},
           {"denoise", {{"type", "boolean"}, {"description", "default = editor setting"}}},
           {"denoiseStrength", {{"type", "number"}, {"description", "0-1, default = editor setting"}}},
           {"aperture", {{"type", "number"}, {"description", "lens radius, 0-1, 0 = no DoF"}}},
           {"focusDist", {{"type", "number"}, {"description", "focus distance in world units"}}},
           {"focalPoint",
            {{"type", "array"},
             {"items", {{"type", "number"}}},
             {"description", "explicit pose: orbit target [x,y,z], default = editor camera"}}},
           {"distance", {{"type", "number"}, {"description", "explicit pose: orbit distance, min 0.05"}}},
           {"pitchDeg", {{"type", "number"}, {"description", "explicit pose: -88 to 88"}}},
           {"yawDeg", {{"type", "number"}, {"description", "explicit pose: degrees"}}},
           {"fovDeg", {{"type", "number"}, {"description", "explicit pose: 10-120"}}}}},
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
            // Per-render overrides live on the job (or one-shot PathTracer state);
            // editor members stay untouched, and the first interactive
            // UpdateRayTracer after the job re-applies them anyway.
            // #138: hermetic pose — the optional pose args land on a copy of
            // the editor camera, so the render neither depends on nor disturbs
            // whatever the live viewport camera does mid-session.
            EditorCamera cam = m_Camera;
            ApplyPoseArgs(args, cam);
            const float aperture = std::clamp(args.value("aperture", 0.0f), 0.0f, 1.0f);
            float focusDist = args.value("focusDist", -1.0f);
            if (focusDist <= 0.0f)
                focusDist = glm::length(cam.FocalPoint() - cam.Position());
            const mat4& view = cam.View();
            m_PathTracer.SetLens(aperture, focusDist,
                                 vec3(view[0][0], view[1][0], view[2][0]),
                                 vec3(view[0][1], view[1][1], view[2][1]));
            m_PathTracer.SetDenoise(args.value("denoise", m_Denoise),
                                    std::clamp(args.value("denoiseStrength", m_DenoiseStrength),
                                               0.0f, 1.0f));
            m_PathTracer.ResetAccumulation();
            // #138: identical scene + pose + spp must reproduce identical
            // pixels; the RNG frame counter restarts so every MCP render
            // walks the same sample sequence.
            m_PathTracer.ResetFrameCounter();

            m_McpRender.active = true;
            m_McpRender.bounces = std::clamp(args.value("bounces", m_Bounces), 1, 16);
            m_McpRender.sppTarget = std::clamp(args.value("spp", 256), 8, 4096);
            // Projection rebuilt for the requested aspect; the viewport's own
            // matrix would letterbox-stretch anything non-viewport-shaped.
            m_McpRender.viewProj = glm::perspective(glm::radians(cam.FOV()),
                                                    (float)w / (float)h, 0.1f, 1000.0f) *
                                   view;
            m_McpRender.camPos = cam.Position();
            m_McpRender.respond = std::move(respond);
        });

    // --- actuation (#77): every mutating action lands on the CommandStack ----

    m_McpProtocol.RegisterTool(
        "manage_entity",
        "Create and organize scene entities. Actions: spawn (primitive: cube|sphere|plane|"
        "cylinder|cone|torus|lathe|sweep|text, optional position/rotationDeg/scale/albedo/"
        "params), "
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
             {"enum",
              {"cube", "sphere", "plane", "cylinder", "cone", "torus", "lathe", "sweep",
               "text"}}}},
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
              "lathe {profile:[[r,y],...] bottom->top revolved around local +Y, sectors, "
              "closed} — walls come from a profile that goes up the outside and back down "
              "the inside; sweep {profile:[[x,y],...] closed cross-section, path:[[x,y,z],"
              "...]} extruded along the path without twist — the section reads as drawn "
              "looking back along the start tangent, world +Y up: a +z path maps section "
              "(x,y) to world (x,y); vertical paths map to world (x,-z) going up, "
              "(x,z) going down (#138); "
              "text {text,depth,fontPath}"}}}}},
         {"required", {"action"}}},
        [this](const json& args) { return ToolManageEntity(args); });

    m_McpProtocol.RegisterTool(
        "manage_material",
        "Set material properties on an entity (any subset): albedo [r,g,b] 0-1, metallic, "
        "roughness, emissive [r,g,b], emissiveStrength, transmission (0=solid 1=glass), ior, "
        "subsurface (0=opaque 1=full SSS; ray traced only), subsurfaceColor [r,g,b], "
        "subsurfaceRadius [r,g,b] (scatter depth per channel, world units — red deepest for "
        "skin/wax), albedoTexture (image file path; empty string clears). slot selects a "
        "material slot on multi-material meshes (default 0 = base; imported glTF meshes may "
        "have more — see materialSlots in get_entity). Undoable.",
        {{"type", "object"},
         {"properties",
          {{"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"slot", {{"type", "integer"}}},
           {"albedo", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"metallic", {{"type", "number"}}},
           {"roughness", {{"type", "number"}}},
           {"emissive", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"emissiveStrength", {{"type", "number"}}},
           {"transmission", {{"type", "number"}}},
           {"ior", {{"type", "number"}}},
           {"subsurface", {{"type", "number"}}},
           {"subsurfaceColor", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"subsurfaceRadius", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"albedoTexture", {{"type", "string"}}}}}},
        [this](const json& args) { return ToolManageMaterial(args); });

    m_McpProtocol.RegisterTool(
        "set_texture",
        "Set or clear a texture on an entity's material (#113). slot: 'albedo' (sRGB color, "
        "multiplies the albedo factor) or 'roughness' (grayscale baked into the metallic-"
        "roughness map's G channel; roughness factor multiplies it, metallic passes through). "
        "source: an image file path string, or a procedural recipe object {kind: 'checker'|"
        "'stripes'|'gradient'|'noise'|'wood', resolution 16-4096 (default 512), seed, "
        "colorA/colorB [r,g,b] linear 0-1, scale (checker cells / stripe pairs / noise cells / "
        "wood rings; wood also accepts ringScale), octaves 1-8 (noise detail), distort (wood "
        "grain distortion; alias grainNoise), ratio (stripes colorA fraction), axis 'u'|'v' "
        "(stripes/gradient direction)}. Checker/stripes/noise tile; recipes bake CPU-side, "
        "persist in .forge saves, and are sampled by BOTH the raster viewport and the path "
        "tracer. Meaningful results need UVs — run edit_mesh unwrap_uv first on meshes "
        "without them. clear:true removes the map. materialSlot selects the material slot on "
        "multi-material meshes (default 0). Undoable.",
        {{"type", "object"},
         {"properties",
          {{"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"slot", {{"type", "string"}, {"enum", {"albedo", "roughness"}}}},
           {"materialSlot", {{"type", "integer"}}},
           {"source",
            {{"description", "file path string or procedural recipe object"},
             {"type", {"string", "object"}}}},
           {"clear", {{"type", "boolean"}}}}}},
        [this](const json& args) { return ToolSetTexture(args); });

    // Async like render_image: network + disk run on a worker thread, the
    // GL-side apply happens on the main thread in UpdatePolyHaven (#84).
    m_McpProtocol.RegisterToolAsync(
        "search_polyhaven",
        "Search the Poly Haven CC0 asset library (#84). type: 'hdri' (real environment "
        "skies for image-based lighting), 'texture' (photographed PBR material sets), "
        "'model' (glTF props). query matches name/tags/categories; empty query lists the "
        "most downloaded. Returns id/name/categories/tags/downloads/maxResolution per "
        "asset — feed an id to download_polyhaven_asset. Needs internet on first use; "
        "the catalog is cached and reused (marked stale:true when served offline).",
        {{"type", "object"},
         {"properties",
          {{"type", {{"type", "string"}, {"enum", {"hdri", "texture", "model"}}}},
           {"query", {{"type", "string"}}},
           {"limit", {{"type", "integer"}, {"description", "max results, default 15, cap 50"}}}}},
         {"required", {"type"}}},
        [this](const json& args, ToolResponder respond) {
            StartPolyHavenSearch(args, std::move(respond));
        });

    m_McpProtocol.RegisterToolAsync(
        "download_polyhaven_asset",
        "Download a Poly Haven asset into the local cache and apply it (#84). type 'hdri': "
        "loads the .hdr as the environment sky + IBL (like set_environment; not undoable). "
        "type 'texture': fetches the Diffuse and Rough maps; pass entity id/name (+ optional "
        "materialSlot) to apply them as albedo + roughness textures (undoable; the mesh "
        "needs UVs — unwrap_uv first), otherwise the cached file paths are returned for "
        "set_texture. type 'model': imports the glTF (with its buffers/textures) into the "
        "scene. resolution '1k'..'8k', default '2k'; the nearest published step is used. "
        "Repeat downloads hit the cache and work offline. The asset id is recorded in the "
        "scene for provenance.",
        {{"type", "object"},
         {"properties",
          {{"asset", {{"type", "string"}, {"description", "asset id from search_polyhaven"}}},
           {"type", {{"type", "string"}, {"enum", {"hdri", "texture", "model"}}}},
           {"resolution", {{"type", "string"}, {"description", "'1k'..'8k', default '2k'"}}},
           {"id", {{"type", "string"}, {"description", "target entity for texture apply"}}},
           {"name", {{"type", "string"}, {"description", "target entity for texture apply"}}},
           {"materialSlot", {{"type", "integer"}}}}},
         {"required", {"asset", "type"}}},
        [this](const json& args, ToolResponder respond) {
            StartPolyHavenDownload(args, std::move(respond));
        });

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
        "union|subtract|intersect with otherId/otherName; replaces both operands — the "
        "result keeps the target's name and id, so chained cuts need no re-lookup, #138), "
        "remesh "
        "(detail 32-160), mirror (bake X-mirror), extrude_face (pick a face via u/v or "
        "origin/direction ray on the target, push it out by distance), unwrap_uv (generate "
        "a non-overlapping UV atlas with xatlas; optional target resolution 256-4096, "
        "default 1024 — the packed page can differ slightly, actual size/charts/"
        "utilization returned in result.atlas; run before assigning image textures). "
        "All undoable.",
        {{"type", "object"},
         {"properties",
          {{"action",
            {{"type", "string"},
             {"enum",
              {"subdivide", "smooth", "boolean", "remesh", "mirror", "extrude_face",
               "unwrap_uv"}}}},
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
           {"distance", {{"type", "number"}}},
           {"resolution", {{"type", "integer"}}}}},
         {"required", {"action"}}},
        [this](const json& args) { return ToolEditMesh(args); });

    m_McpProtocol.RegisterTool(
        "manage_scene",
        "Scene-level ops. Actions: new (clears the scene, discards unsaved changes), open "
        "(path to .forge), save (optional path; required when untitled), import_model (path "
        "to .gltf/.glb/.obj), look_at (frame an entity by id/name, a point, or with no "
        "target the whole scene; optional distance), undo, redo, set_render_settings "
        "(viewer state, saved with the scene, "
        "not undoable: rayTracing on/off, bounces 1-16, rtScale 0.25-1, denoise, "
        "denoiseStrength 0-1, aperture, focusDist; any subset; returns current values).",
        {{"type", "object"},
         {"properties",
          {{"action",
            {{"type", "string"},
             {"enum", {"new", "open", "save", "import_model", "look_at", "undo", "redo",
                       "set_render_settings"}}}},
           {"path", {{"type", "string"}}},
           {"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"point", {{"type", "array"}, {"items", {{"type", "number"}}}}},
           {"distance", {{"type", "number"}}},
           {"rayTracing", {{"type", "boolean"}}},
           {"bounces", {{"type", "integer"}}},
           {"rtScale", {{"type", "number"}}},
           {"denoise", {{"type", "boolean"}}},
           {"denoiseStrength", {{"type", "number"}}},
           {"aperture", {{"type", "number"}}},
           {"focusDist", {{"type", "number"}}}}},
         {"required", {"action"}}},
        [this](const json& args) { return ToolManageScene(args); });

    m_McpProtocol.RegisterTool(
        "render_views",
        "Fast raster multi-view diagnostics (#93) — the critic's eyes. Presets: "
        "turntable (front/right/back/left), 4up (front/right/top-ortho/three-"
        "quarter), top_ortho (plan view). Modes: beauty (lit raster), clay (uniform "
        "gray, geometry-only), wireframe, normals (RGB-encoded world normals — "
        "spot inverted faces), object_id (flat unique color per entity + legend — "
        "name what you see), section (#114: clay cut open at a world plane, cut "
        "faces amber — wall thickness and interior profiles at a glance). Frames "
        "one entity (id/name, with children) or the whole scene. Returns one PNG "
        "per view with labels. Use after every build step: single-view checks miss "
        "floating/interpenetrating parts.",
        {{"type", "object"},
         {"properties",
          {{"preset",
            {{"type", "string"}, {"enum", {"turntable", "4up", "top_ortho"}},
             {"description", "default 4up"}}},
           {"mode",
            {{"type", "string"},
             {"enum", {"beauty", "clay", "wireframe", "normals", "object_id", "section"}},
             {"description", "default clay"}}},
           {"plane",
            {{"type", "object"},
             {"description",
              "section mode: cut plane {origin:[x,y,z], normal:[x,y,z]}; geometry on "
              "the normal's negative side is removed. Default: z=target-center plane, "
              "normal -z — the cut face points at the presets' front camera"}}},
           {"id", {{"type", "string"}}},
           {"name", {{"type", "string"}}},
           {"size", {{"type", "integer"}, {"description", "px per view, 128-768, default 448"}}}}},
         {"additionalProperties", false}},
        [this](const json& args) { return ToolRenderViews(args); });

    // --- scripting (#78): code-as-actuation escape hatch ----------------------

    m_McpProtocol.RegisterTool(
        "execute_script",
        "Run a sandboxed Lua 5.4 script against the scene — loops, math, and "
        "variables for parametric builds that would take dozens of tool calls. "
        "The forge.* functions mirror the other tools and take one table of the "
        "same named fields (vectors as {x,y,z} arrays). Reads: scene(), "
        "get_entity{}, mesh_stats{}, raycast{}, check_overlap{}, query_spatial{}, "
        "measure{a/b or entity+axis — #114 distances/extents}, compare_silhouette{"
        "reference, view — returns IoU numbers incl. regions[]; or "
        "references={{path,view},...} for the multi-view gate, #137 — returns "
        "combined{iou=min}+views[]; the diff images are MCP-only}, "
        "analyze_reference{image — outline + landmarks in model space, #135}. "
        "Writes: spawn{} (incl. primitive='lathe' params={profile={{r,y},... bottom->top "
        "revolved around local +Y}, sectors, closed} and primitive='sweep' "
        "params={profile={{x,y},...} closed section, path={{x,y,z},...}} — crisp cups/"
        "vases/legs/handles instead of sphere-pushing; a +z path maps section (x,y) to "
        "world (x,y), #138), delete{}, "
        "duplicate{}, rename{}, set_transform{}, set_parent{}, set_material{slot for "
        "multi-material meshes; subsurface/subsurfaceColor/subsurfaceRadius = SSS, #112}, "
        "set_texture{slot='albedo'|'roughness', source=file path "
        "or procedural recipe {kind='checker'|'stripes'|'gradient'|'noise'|'wood', colorA, "
        "colorB, scale, ...} — wood grain, glaze bands; needs UVs (unwrap_uv first)}, "
        "spawn_point_light{}, set_point_light{}, set_sun{}, set_environment{}, "
        "place_relative{}, snap_to_surface{}, align{}, distribute{}, "
        "subdivide{}, smooth{}, boolean{} (result keeps the target's name+id, #138), "
        "remesh{}, mirror{}, extrude_face{}, "
        "unwrap_uv{optional resolution 256-4096} (non-overlapping UV atlas — run "
        "before assigning image textures), "
        "look_at{}, set_render_settings{}. Editor surface (#91, script-only): "
        "camera{}/set_camera{}/store_view{}/recall_view{} (orbit pose, FOV, "
        "bookmarks 1-4), select{}/toggle_select{}/clear_selection{}/"
        "get_selection{}/box_select{} (viewport-UV marquee), group{}/ungroup{}/"
        "drop_to_ground{}, snap_settings{}, mesh_elements{} (face/edge ids + "
        "world centers; face id = raycast triIndex/3; ofFaces maps faces to "
        "their edges), extrude_faces{}/extrude_edges{}/subdivide_faces{}/"
        "subdivide_edges{}/shade{} (edit-mode element ops by id), "
        "sculpt{id, brush='grab'|'inflate'|'smooth', center={x,y,z}, radius, "
        "strength, direction (grab), strokes=N, mirror=bool, snap=bool, "
        "relax=bool} (brush at a world point; strength = world units for "
        "grab/inflate, negative inflate dents, 0..1 relax factor for smooth; "
        "mirror repeats across the local YZ plane for symmetric work, snap "
        "projects the center onto the nearest surface vertex — use it instead "
        "of guessing coordinates, relax smooths the region first; a warning "
        "field flags folded regions whose normals oppose the brush), "
        "move_verts{id, point, radius, offset, "
        "falloff='smooth'|'linear'|'constant', mirror, snap, relax} "
        "(falloff-weighted vertex translation, weld-seam safe), "
        "set_pose{id, joint='LeftForeArm', euler=[x,y,z] deg | quat=[x,y,z,w]} bends one "
        "joint (delta from bind, identity=rest; needs a skinned/imported model), or "
        "set_pose{id, preset='rest'|'t-pose'|'a-pose'|'sit'} applies a canned pose to "
        "matching joints; undo stores only the pose, and the path tracer re-skins after "
        "the settle. "
        "export_stl{}. "
        "Entity writes return the affected entity as a table (use .id); camera/"
        "selection/element/export calls return their own shapes. "
        "print() lines and the script's return value come back in the result. "
        "The whole script is ONE undo entry; on error the partial build rolls "
        "back (sun/environment/camera/snap-settings changes and written files "
        "excepted). Sandboxed: no os/io/require; runaway loops and memory "
        "bombs abort.",
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
           // ~atlas utilization when charts don't overlap; >1 = stacked UVs (#81)
           {"uvAreaCoverage", UvAreaCoverage(e->mesh->Vertices(), e->mesh->Indices())},
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

ToolResult EditorApp::ToolCheckOverlap(const json& args)
{
    std::string error;
    Entity* a = FindToolTarget(m_Scene, args, error);
    if (!a)
        return Err(error);
    Entity* b = FindToolTargetKeyed(m_Scene, args, "otherId", "otherName", error);
    if (!b)
        return Err(error);
    if (a->id == b->id)
        return Err("Both references resolve to the same entity \"" + a->name + "\"");
    const AABB wa = WorldBoundsOf(m_Scene, *a);
    const AABB wb = WorldBoundsOf(m_Scene, *b);
    if (!wa.Valid())
        return Err("Entity \"" + a->name + "\" has no mesh (no bounds)");
    if (!wb.Valid())
        return Err("Entity \"" + b->name + "\" has no mesh (no bounds)");

    const OverlapResult r = OverlapAABB(wa, wb);
    json j{{"overlap", r.overlap},
           {"a", {{"id", std::to_string(a->id)}, {"name", a->name},
                  {"min", Vec3Json(wa.min)}, {"max", Vec3Json(wa.max)}}},
           {"b", {{"id", std::to_string(b->id)}, {"name", b->name},
                  {"min", Vec3Json(wb.min)}, {"max", Vec3Json(wb.max)}}}};
    if (r.overlap) {
        j["penetration"] = Vec3Json(r.penetration);
        j["depth"] = r.depth;
    } else {
        j["distance"] = r.distance;
    }
    return JsonResult(j);
}

// --- measurement & critique (#114) ------------------------------------------------

bool EditorApp::ResolveMeasurePoint(const json& endpoint, vec3& out, std::string& error)
{
    if (Vec3FromJson(endpoint, out))
        return true;
    if (!endpoint.is_object()) {
        error = "expected [x,y,z] or {entity/id/name, feature}";
        return false;
    }
    Entity* e = endpoint.contains("entity")
                    ? FindEntityFlexible(m_Scene, endpoint["entity"], error)
                    : FindToolTarget(m_Scene, endpoint, error);
    if (!e)
        return false;
    // Landmarks live on the subtree's world AABB: a group measures as the
    // whole assembly, a leaf mesh as itself.
    const AABB wb = SubtreeWorldBounds(e->id, nullptr);
    if (!wb.Valid()) {
        error = "entity \"" + e->name + "\" has no mesh in its subtree";
        return false;
    }
    const std::string feature = endpoint.value("feature", "center");
    const std::optional<vec3> p = AabbLandmark(wb, feature);
    if (!p) {
        error = "unknown feature \"" + feature + "\" (top, bottom, or center)";
        return false;
    }
    out = *p;
    return true;
}

ToolResult EditorApp::ToolMeasure(const json& args)
{
    const bool hasA = args.contains("a"), hasB = args.contains("b");
    if (hasA != hasB)
        return Err("Provide both \"a\" and \"b\" for a distance, or entity/id/name for extents");

    if (hasA) {
        vec3 pa, pb;
        std::string error;
        if (!ResolveMeasurePoint(args["a"], pa, error))
            return Err("a: " + error);
        if (!ResolveMeasurePoint(args["b"], pb, error))
            return Err("b: " + error);
        const vec3 d = pb - pa;
        return JsonResult(json{{"distance", glm::length(d)},
                               {"delta", Vec3Json(d)},
                               {"a", Vec3Json(pa)},
                               {"b", Vec3Json(pb)}});
    }

    std::string error;
    Entity* e = args.contains("entity") ? FindEntityFlexible(m_Scene, args["entity"], error)
                                        : FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);
    const AABB wb = SubtreeWorldBounds(e->id, nullptr);
    if (!wb.Valid())
        return Err("Entity \"" + e->name + "\" has no mesh in its subtree (no bounds)");

    json j{{"id", std::to_string(e->id)}, {"name", e->name}};
    if (args.contains("axis")) {
        const std::string axis = args.value("axis", "");
        const std::optional<float> extent = AabbAxisExtent(wb, axis);
        if (!extent)
            return Err("Unknown axis \"" + axis + "\" (x, y, or z)");
        const int i = axis == "x" ? 0 : axis == "y" ? 1 : 2;
        j["axis"] = axis;
        j["extent"] = *extent;
        j["min"] = wb.min[i];
        j["max"] = wb.max[i];
    } else {
        j["extents"] = Vec3Json(wb.max - wb.min);
        j["min"] = Vec3Json(wb.min);
        j["max"] = Vec3Json(wb.max);
        j["center"] = Vec3Json((wb.min + wb.max) * 0.5f);
    }
    return JsonResult(j);
}

ToolResult EditorApp::ToolCompareSilhouette(const json& args)
{
    // Two input forms (#137): the original single reference (+ optional view),
    // or references=[{path, view}] gating the same target against several
    // photos. Mutually exclusive — the registry's simple schemas can't express
    // oneOf, so the handler owns the check.
    const bool multiView = args.contains("references");
    if (multiView && (args.contains("reference") || args.contains("view")))
        return Err("Use either reference/view or references[], not both");

    struct RefEntry {
        std::string path;
        std::string view;
    };
    std::vector<RefEntry> entries;
    if (multiView) {
        const json& refs = args["references"];
        if (!refs.is_array() || refs.empty())
            return Err("references must be a non-empty array of {path, view}");
        if (refs.size() > 8)
            return Err("references accepts at most 8 entries");
        // Validate the WHOLE batch up front: the run below is all-or-nothing,
        // so no entry may start work while a later one is malformed. Duplicate
        // views are fine — two photos of the same face are legitimate.
        for (size_t i = 0; i < refs.size(); ++i) {
            const json& r = refs[i];
            const std::string at = "references[" + std::to_string(i) + "]";
            if (!r.is_object())
                return Err(at + " must be an object {path, view}");
            if (!r.contains("path") || !r["path"].is_string() ||
                r["path"].get<std::string>().empty())
                return Err(at + ".path must be a non-empty string (reference image path)");
            if (!r.contains("view") || !r["view"].is_string() ||
                !IsSilhouetteView(r["view"].get<std::string>()))
                return Err(at + ".view must be one of front, back, left, right, top");
            entries.push_back({r["path"].get<std::string>(), r["view"].get<std::string>()});
        }
    } else {
        if (!args.contains("reference") || !args["reference"].is_string())
            return Err("Provide \"reference\" (path to the reference image)");
        entries.push_back({args["reference"].get<std::string>(), args.value("view", "front")});
    }

    const double thresholdRaw = args.value("threshold", 0.8);
    if (!(thresholdRaw >= 0.0 && thresholdRaw <= 1.0))
        return Err("threshold must be in [0, 1]");
    const float threshold = (float)thresholdRaw; // shared by every view
    const int size = std::clamp(args.value("size", 256), 64, 1024);

    // Draw set: the target subtree, or every mesh in the scene — resolved once
    // and shared across views. Light-gizmo spheres are skipped like in
    // render_views — they'd bleed into the outline.
    std::unordered_set<UUID> subtree;
    if (args.contains("id") || args.contains("name")) {
        std::string error;
        Entity* e = FindToolTarget(m_Scene, args, error);
        if (!e)
            return Err(error);
        for (UUID node : SubtreeOf(e->id))
            subtree.insert(node);
    }
    AABB bounds;
    std::vector<const Entity*> targets;
    for (const Entity& e : m_Scene.Entities()) {
        if (!e.mesh || e.light.enabled)
            continue;
        if (!subtree.empty() && !subtree.count(e.id))
            continue;
        const AABB b = WorldBoundsOf(m_Scene, e);
        if (!b.Valid())
            continue;
        bounds.Expand(b.min);
        bounds.Expand(b.max);
        targets.push_back(&e);
    }
    if (!bounds.Valid())
        return Err("Nothing to compare (no meshes)");

    // One reference/view compare — the whole single-view pipeline including
    // the #136 regions/symmetry tail. Both argument forms run THIS lambda, so
    // the shapes cannot drift apart; each call derives its own viewProj,
    // framing transform, and depth plane — no state crosses views (#137).
    struct ViewCompare {
        json block;                // the single-view result JSON (iou..symmetry)
        float iou = 0.0f;          // lifted out for CombineViewScores
        std::string silhouettePng; // base64; empty = PNG encode failed, skip
        std::string diffPng;
        std::string error; // non-empty = this compare failed; call fails whole
    };
    auto compareOneView = [&](const std::string& view, const std::string& refPath) {
        ViewCompare out;
        const std::optional<mat4> viewProj = SilhouetteViewProj(view, bounds);
        if (!viewProj) {
            // bounds are valid here, so only the view name can be at fault.
            out.error = "Unknown view \"" + view + "\" (front, back, left, right, or top)";
            return out;
        }

        // Software rasterizer, not the GL renderer: exact mesh coverage, no
        // shading/AA noise in a binary mask, and the whole path is unit-tested.
        SilhouetteMask rendered = MakeMask(size, size);
        for (const Entity* e : targets)
            RasterizeSilhouette(e->mesh->Vertices(), e->mesh->Indices(),
                                *viewProj * m_Scene.WorldTransform(e->id), rendered);
        if (MaskArea(rendered) == 0) {
            out.error = "Rendered silhouette is empty (degenerate geometry?)";
            return out;
        }

        int refW = 0, refH = 0, refC = 0;
        stbi_uc* refPixels = stbi_load(refPath.c_str(), &refW, &refH, &refC, 4);
        if (!refPixels) {
            out.error = "Failed to load reference image \"" + refPath +
                        "\": " + stbi_failure_reason();
            return out;
        }
        // RAII on the C buffer: BinarizeImage allocates image-sized transients,
        // and a bad_alloc there unwinds to the protocol layer (which keeps the
        // app running) — the decode buffer must not leak on that path.
        const std::unique_ptr<stbi_uc, void (*)(void*)> refOwner(refPixels, stbi_image_free);
        SilhouetteMask reference = BinarizeImage(refPixels, refW, refH);
        if (MaskArea(reference) == 0) {
            out.error = "Reference image has no foreground after binarization";
            return out;
        }

        // The render's framing transform is kept: mismatch regions found in the
        // normalized frame map back through it (then the viewport inverse) into
        // the MODEL's world coordinates — where to grow/trim (#136).
        NormalizeTransform renderXform;
        const SilhouetteMask a = NormalizeMask(rendered, size, renderXform);
        const SilhouetteMask b = NormalizeMask(reference, size);
        const SilhouetteDiff diff = CompareMasks(a, b);
        const DiffRegionList regionList = DiffRegions(a, b); // defaults: 16 regions, 0.2% speck floor

        json j{{"iou", diff.iou},
               {"dice", diff.dice},
               {"pass", diff.iou >= threshold},
               {"threshold", threshold},
               {"view", view},
               {"pixels",
                {{"match", diff.intersection},
                 {"renderOnly", diff.onlyA},
                 {"referenceOnly", diff.onlyB}}}};

        // Depth plane for world mapping: the target's bounds center projected
        // through the same viewProj. In-plane axes are exact for the ortho views;
        // the depth coordinate is a stated convention, not information.
        const vec4 centerClip = *viewProj * vec4((bounds.min + bounds.max) * 0.5f, 1.0f);
        const float ndcZ = centerClip.z / centerClip.w;
        auto toWorld = [&](vec2 normPx) {
            return MaskPxToWorld(*viewProj, rendered.width, rendered.height,
                                 NormalizedToSourcePx(renderXform, normPx), ndcZ);
        };
        json regionsJson = json::array();
        for (const DiffRegion& r : regionList.regions) {
            const vec3 cw = toWorld(r.centroid);
            // Exclusive far corner so the box covers the last pixel; per-component
            // min/max absorbs the image-vs-world y flip without view casing.
            const vec3 c0 = toWorld(vec2((float)r.minX, (float)r.minY));
            const vec3 c1 = toWorld(vec2((float)(r.maxX + 1), (float)(r.maxY + 1)));
            const vec3 lo = glm::min(c0, c1), hi = glm::max(c0, c1);
            regionsJson.push_back({{"type", r.excess ? "excess" : "missing"},
                                   {"areaFraction", r.areaFraction},
                                   {"areaPx", r.area},
                                   {"centroidPx", {r.centroid.x, r.centroid.y}},
                                   {"bboxPx", {r.minX, r.minY, r.maxX, r.maxY}},
                                   {"centroidWorld", Vec3Json(cw)},
                                   {"bboxWorld", {{"min", Vec3Json(lo)}, {"max", Vec3Json(hi)}}}});
        }
        j["regions"] = regionsJson;
        j["regionSummary"] = {{"total", regionList.totalRegions},
                              {"dropped", regionList.droppedRegions},
                              {"droppedAreaPx", regionList.droppedArea}};
        j["symmetry"] = {{"render", MaskMirrorSymmetryX(a)},
                         {"reference", MaskMirrorSymmetryX(b)}};
        out.iou = diff.iou;
        out.block = std::move(j);

        std::vector<uint8_t> silhouetteImg((size_t)size * size * 4);
        for (size_t i = 0; i < a.pixels.size(); ++i) {
            const uint8_t v = a.pixels[i] ? 255 : 0;
            silhouetteImg[i * 4 + 0] = silhouetteImg[i * 4 + 1] = silhouetteImg[i * 4 + 2] = v;
            silhouetteImg[i * 4 + 3] = 255;
        }
        out.silhouettePng = PixelsToPngBase64(silhouetteImg.data(), size, size);
        const std::vector<uint8_t> diffImg = DiffImageRGBA(a, b);
        if (!diffImg.empty())
            out.diffPng = PixelsToPngBase64(diffImg.data(), size, size);
        return out;
    };

    // The labeled image pair for one compare. The single form keeps today's
    // exact labels (prefix empty); the array form prefixes "[view] " so the
    // stream stays readable when several diffs follow one JSON block.
    auto pushImages = [](ToolResult& result, const ViewCompare& vc, const std::string& prefix) {
        if (!vc.silhouettePng.empty()) {
            result.content.push_back(
                {{"type", "text"}, {"text", prefix + "silhouette (normalized render mask)"}});
            result.content.push_back(
                {{"type", "image"}, {"data", vc.silhouettePng}, {"mimeType", "image/png"}});
        }
        if (!vc.diffPng.empty()) {
            result.content.push_back(
                {{"type", "text"},
                 {"text",
                  prefix + "diff (gray = match, green = render only, magenta = reference only)"}});
            result.content.push_back(
                {{"type", "image"}, {"data", vc.diffPng}, {"mimeType", "image/png"}});
        }
    };

    if (!multiView) {
        // Single form: the helper's block IS the top-level JSON — byte-identical
        // to the pre-#137 tool (existing scripts and the closed-loop demo pin
        // this shape; no combined/views keys here).
        ViewCompare vc = compareOneView(entries[0].view, entries[0].path);
        if (!vc.error.empty())
            return Err(vc.error);
        ToolResult result;
        // Metrics text first: the Lua binding surfaces content[0] only, so
        // forge.compare_silhouette returns the numbers (images are MCP-only).
        result.content.push_back({{"type", "text"}, {"text", vc.block.dump(2)}});
        pushImages(result, vc, "");
        return result;
    }

    // Array form: run every view, all-or-nothing — results buffer until the
    // whole batch succeeded, so a mid-batch failure emits no partial content.
    std::vector<ViewCompare> compares;
    compares.reserve(entries.size());
    std::vector<float> ious;
    ious.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) {
        ViewCompare vc = compareOneView(entries[i].view, entries[i].path);
        if (!vc.error.empty())
            return Err("references[" + std::to_string(i) + "] (\"" + entries[i].path +
                       "\"): " + vc.error);
        // The path rides along so an agent can tell entries apart when views
        // repeat; everything else matches the single-view block exactly.
        vc.block["reference"] = entries[i].path;
        ious.push_back(vc.iou);
        compares.push_back(std::move(vc));
    }

    // min across views is the honest gate — a replica with a failing side view
    // is not a passing replica; the mean is reported for trend reading (#137).
    const ViewScoreSummary combined = CombineViewScores(ious, threshold);
    json j{{"combined",
            {{"iou", combined.minIou},
             {"meanIou", combined.meanIou},
             {"pass", combined.allPass},
             {"threshold", threshold}}},
           {"views", json::array()}};
    for (ViewCompare& vc : compares)
        j["views"].push_back(std::move(vc.block));

    ToolResult result;
    result.content.push_back({{"type", "text"}, {"text", j.dump(2)}});
    for (size_t i = 0; i < compares.size(); ++i)
        pushImages(result, compares[i], "[" + entries[i].view + "] ");
    return result;
}

ToolResult EditorApp::ToolAnalyzeReference(const json& args)
{
    if (!args.contains("image") || !args["image"].is_string())
        return Err("Provide \"image\" (path to the reference image)");
    const std::string imagePath = args["image"];
    // Lua numbers can arrive as doubles; read leniently, then clamp.
    auto numArg = [&](const char* key, double def) {
        return args.contains(key) && args[key].is_number() ? args[key].get<double>() : def;
    };
    const int maxPoints = std::clamp((int)std::lround(numArg("maxPoints", 64.0)), 8, 512);
    const int holeBudget = std::min(maxPoints, 32); // holes/extras are supporting geometry
    const float tolerancePx = (float)std::clamp(numArg("tolerancePx", 1.0), 0.0, 50.0);
    const int spanRows = std::clamp((int)std::lround(numArg("spanRows", 0.0)), 0, 4096);
    std::string mirror;
    if (args.contains("mirror")) {
        if (!args["mirror"].is_string())
            return Err("mirror must be the string \"x\"");
        mirror = args["mirror"];
        if (mirror != "x")
            return Err("mirror must be \"x\" (fold about the vertical axis) or omitted");
    }

    int refW = 0, refH = 0, refC = 0;
    stbi_uc* refPixels = stbi_load(imagePath.c_str(), &refW, &refH, &refC, 4);
    if (!refPixels)
        return Err("Failed to load reference image \"" + imagePath +
                   "\": " + stbi_failure_reason());
    // RAII on the C buffer (#114): the kernel allocates image-sized transients,
    // and a bad_alloc there must not leak the decode buffer.
    const std::unique_ptr<stbi_uc, void (*)(void*)> refOwner(refPixels, stbi_image_free);

    const SilhouetteMask mask = BinarizeImage(refPixels, refW, refH);
    if (MaskArea(mask) == 0)
        return Err("Reference image has no foreground after binarization");

    const SilhouetteLandmarks lm = MaskLandmarks(mask);
    // Area floor (#142 review): a grainy scan or dithered reference peppers
    // thousands of pinhole loops, and the kernel's parent pass is quadratic in
    // loop count — on the GL main thread, with no cancel. Sub-speck loops
    // carry no usable shape at this scale; the floor's cost shows up honestly
    // in roundTripIoU.
    std::vector<SilhouetteContour> contours =
        TraceContours(mask, std::max(4.0, 1e-4 * (double)lm.area));
    // Largest top-level outer = THE outline; the rest of the top-level outers are
    // extra outlines. Holes attach to their parent.
    int outlineIdx = -1;
    double bestArea = -1.0;
    for (int i = 0; i < (int)contours.size(); ++i)
        if (contours[i].parent == -1 && !contours[i].hole && std::abs(contours[i].area) > bestArea) {
            bestArea = std::abs(contours[i].area);
            outlineIdx = i;
        }
    if (outlineIdx < 0)
        return Err("Reference produced no outer contour");

    // Bbox-centered model space: x right, y UP, larger bbox side spans 1.0, so
    // the outline lands in [-0.5, 0.5]^2 — sweep-section- and lathe-ready.
    const double cx = (lm.minX + lm.maxX + 1) / 2.0; // corner coords
    const double cy = (lm.minY + lm.maxY + 1) / 2.0;
    const double s = 1.0 / (double)std::max(lm.bboxWidth, lm.bboxHeight);
    auto round5 = [](double v) { return std::round(v * 100000.0) / 100000.0; };
    auto modelX = [&](double px) { return (px - cx) * s; };
    auto modelY = [&](double py) { return (cy - py) * s; }; // flip to y-up
    auto modelPt = [&](const vec2& p) -> json {
        return {round5(modelX(p.x)), round5(modelY(p.y))};
    };

    // Parent links -> children lists once (O(contours)), for the recursive
    // emission below (outer -> holes -> islands -> ...).
    std::vector<std::vector<int>> children(contours.size());
    for (int i = 0; i < (int)contours.size(); ++i)
        if (contours[i].parent >= 0)
            children[(size_t)contours[i].parent].push_back(i);

    // Emit one simplified contour: points in model space (rounded), plus area
    // and centroid computed in model space. Every emitted polygon also feeds
    // rasterSet so roundTripIoU scores exactly what the JSON reports.
    std::vector<SilhouetteContour> rasterSet;
    int emittedCount = 0;
    auto emitSimplified = [&](const std::vector<vec2>& simp) -> json {
        // A loop simplified below a triangle would be a "polygon" the
        // rasterizer silently skips — drop it; droppedContours picks it up.
        if (simp.size() < 3)
            return json();
        rasterSet.push_back(SilhouetteContour{simp, 0.0, false, -1});
        ++emittedCount;
        std::vector<vec2> model;
        json pts = json::array();
        model.reserve(simp.size());
        for (const vec2& p : simp) {
            model.push_back(vec2((float)modelX(p.x), (float)modelY(p.y)));
            pts.push_back(modelPt(p));
        }
        const vec2 c = PolygonCentroid(model);
        json jc;
        jc["points"] = pts;
        jc["area"] = round5(std::abs(PolygonSignedArea(model)));
        jc["centroid"] = {round5(c.x), round5(c.y)};
        jc["pointCount"] = (int)simp.size();
        return jc;
    };
    auto emitContour = [&](const std::vector<vec2>& pixelPts, int budget) -> json {
        return emitSimplified(SimplifyContour(pixelPts, budget, tolerancePx));
    };
    // Largest-first order + per-level caps keep the JSON bounded on a
    // pathological mask; droppedContours reports what the caps cut.
    constexpr size_t kMaxHolesPerOuter = 64;
    constexpr size_t kMaxIslandsPerHole = 16; // also caps extraOutlines
    auto largestFirst = [&](std::vector<int>& idx, size_t cap) {
        std::sort(idx.begin(), idx.end(), [&](int a, int b) {
            return std::abs(contours[a].area) > std::abs(contours[b].area);
        });
        if (idx.size() > cap)
            idx.resize(cap);
    };
    // Recursive shape (#142 review): each outer carries its holes, each hole
    // carries the islands inside it, and an island recurses like any outer —
    // without this, an outer contour whose parent is a hole simply vanished
    // from the JSON (and from the round-trip score).
    std::function<json(int, int)> emitOuter; // assigned below; holes need it for islands
    auto attachHoles = [&](int idx, json& outer) {
        std::vector<int> holeIdx;
        for (int c : children[(size_t)idx])
            if (contours[c].hole)
                holeIdx.push_back(c);
        largestFirst(holeIdx, kMaxHolesPerOuter);
        json holes = json::array();
        for (int h : holeIdx) {
            json hj = emitContour(contours[h].points, holeBudget);
            if (hj.is_null())
                continue;
            std::vector<int> islandIdx;
            for (int c : children[(size_t)h])
                if (!contours[c].hole)
                    islandIdx.push_back(c);
            largestFirst(islandIdx, kMaxIslandsPerHole);
            json islands = json::array();
            for (int isl : islandIdx) {
                const json ij = emitOuter(isl, holeBudget);
                if (!ij.is_null())
                    islands.push_back(ij);
            }
            hj["islands"] = islands;
            holes.push_back(hj);
        }
        outer["holes"] = holes;
    };
    emitOuter = [&](int idx, int budget) -> json {
        json outer = emitContour(contours[idx].points, budget);
        if (!outer.is_null())
            attachHoles(idx, outer);
        return outer;
    };

    // Simplify the outer ONCE: the outline emission and the mirror fold below
    // share outerSimp (#142 review nit — they used identical arguments).
    const std::vector<vec2> outerSimp =
        SimplifyContour(contours[outlineIdx].points, maxPoints, tolerancePx);

    json j;
    j["image"] = {{"width", refW}, {"height", refH}};
    j["mask"] = {{"area", lm.area},
                 {"areaFractionImage", lm.areaFractionImage},
                 {"fillFactor", lm.fillFactor}};
    j["bbox"] = {{"x", lm.minX}, {"y", lm.minY},   {"width", lm.bboxWidth},
                 {"height", lm.bboxHeight}, {"aspect", lm.aspect}};
    j["centroid"] = modelPt(lm.centroid);
    j["centroidPx"] = {round5(lm.centroid.x), round5(lm.centroid.y)};
    j["extremities"] = {{"top", modelPt(lm.top)},
                        {"bottom", modelPt(lm.bottom)},
                        {"left", modelPt(lm.left)},
                        {"right", modelPt(lm.right)}};
    j["widestRow"] = {{"y", lm.widestRowY},
                      {"yModel", round5(modelY(lm.widestRowY + 0.5))},
                      {"left", lm.widestRowLeft},
                      {"right", lm.widestRowRight},
                      {"span", lm.widestRowSpan},
                      {"spanModel", round5(lm.widestRowSpan * s)}};
    j["tallestColumn"] = {{"x", lm.tallestColX},
                          {"xModel", round5(modelX(lm.tallestColX + 0.5))},
                          {"top", lm.tallestColTop},
                          {"bottom", lm.tallestColBottom},
                          {"span", lm.tallestColSpan},
                          {"spanModel", round5(lm.tallestColSpan * s)}};

    json mainOutline = emitSimplified(outerSimp);
    if (mainOutline.is_null()) // cannot happen for a traced loop; belt only
        return Err("Reference produced no outer contour");
    attachHoles(outlineIdx, mainOutline);
    j["holes"] = mainOutline["holes"]; // the main outline's holes stay top-level
    mainOutline.erase("holes");
    j["outline"] = mainOutline;

    std::vector<int> extraIdx;
    for (int i = 0; i < (int)contours.size(); ++i)
        if (i != outlineIdx && contours[i].parent == -1 && !contours[i].hole)
            extraIdx.push_back(i);
    largestFirst(extraIdx, kMaxIslandsPerHole); // extras share the 16 cap
    json extra = json::array();
    for (int i : extraIdx) {
        const json e = emitOuter(i, holeBudget);
        if (!e.is_null())
            extra.push_back(e);
    }
    j["extraOutlines"] = extra;
    // Contours traced but not emitted (size caps / degenerate simplification);
    // the kernel's area floor drops upstream of this count and its effect
    // shows up in roundTripIoU instead.
    j["droppedContours"] = (int)contours.size() - emittedCount;

    if (spanRows > 0) {
        json rows = json::array();
        for (const SilhouetteRowSpan& r : MaskRowSpans(mask, spanRows))
            rows.push_back({{"y", r.y},
                            {"yModel", round5(modelY(r.y + 0.5))},
                            {"left", r.left},
                            {"right", r.right},
                            {"covered", r.covered}});
        j["rowSpans"] = rows;
    }

    // Self-reported acceptance: re-rasterize every simplified polygon at the
    // source size and score against the binarized mask (even-odd punches holes).
    const SilhouetteMask rt = RasterizePolygons(rasterSet, refW, refH);
    j["roundTripIoU"] = CompareMasks(rt, mask).iou;

    const double pixelsPerUnit = (double)std::max(lm.bboxWidth, lm.bboxHeight);
    j["scale"] = {{"pixelsPerUnit", pixelsPerUnit},
                  {"modelToPixel", "px = center + point*pixelsPerUnit (y flipped)"}};

    if (mirror == "x") {
        // Fold the simplified outer (outerSimp, shared with the outline
        // emission) in PIXEL space about the bbox center, then map to model
        // space. FoldOutline emits bottom (max y) -> top (min y), which is
        // already bottom -> top in y-up model space — the natural order for a
        // lathe profile, so no reversal is applied.
        const std::vector<vec2> foldPx = FoldOutline(outerSimp, (float)cx);
        std::vector<vec2> hp; // model-space (r, y), bottom -> top
        json hpPts = json::array();
        for (const vec2& p : foldPx) {
            const double rM = (double)p.x * s; // r is a length: scale, no origin shift
            const double yM = modelY(p.y);
            hp.push_back(vec2((float)rM, (float)yM));
            hpPts.push_back({round5(rM), round5(yM)});
        }
        j["halfProfile"] = {{"axisXModel", 0.0}, {"points", hpPts}};
        // A profile whose ends sit off the axis closes to the axis for a lathe.
        if (!hp.empty() && (hp.front().x > 1e-3f || hp.back().x > 1e-3f)) {
            json lp = json::array();
            lp.push_back({0.0, round5(hp.front().y)});
            for (const vec2& p : hp)
                lp.push_back({round5(p.x), round5(p.y)});
            lp.push_back({0.0, round5(hp.back().y)});
            j["latheProfile"] = lp;
        }
    }

    return JsonResult(j);
}

ToolResult EditorApp::ToolQuerySpatial(const json& args)
{
    const std::string action = args.value("action", "");

    // Meshless nodes (groups) still have a pose: fall back to a point at the
    // world position so queries don't silently drop them.
    auto boundsOrPoint = [this](const Entity& e) {
        AABB b = WorldBoundsOf(m_Scene, e);
        if (!b.Valid()) {
            const vec3 p = vec3(m_Scene.WorldTransform(e.id)[3]);
            b.Expand(p);
        }
        return b;
    };

    if (action == "within_radius") {
        if (!args.contains("radius") || !args["radius"].is_number())
            return Err("Provide radius");
        const float radius = args["radius"];
        if (radius < 0.0f)
            return Err("radius must be >= 0");

        // Point form: point-to-box distance. Entity form: box-to-box distance
        // (a chair touching the end of a 20-unit wall is at distance 0, not
        // 10-from-the-midpoint). point wins when both forms are supplied.
        vec3 center;
        AABB centerBounds;
        UUID selfId = 0; // exclude the center entity from its own neighborhood
        const bool pointForm = GetVec3(args, "point", center);
        if (!pointForm) {
            std::string error;
            Entity* e = FindToolTarget(m_Scene, args, error);
            if (!e)
                return Err("Provide point [x,y,z], or id/name of a center entity");
            centerBounds = boundsOrPoint(*e);
            center = (centerBounds.min + centerBounds.max) * 0.5f;
            selfId = e->id;
        }

        std::vector<std::pair<float, const Entity*>> hits;
        for (const Entity& e : m_Scene.Entities()) {
            if (e.id == selfId)
                continue;
            const AABB wb = boundsOrPoint(e);
            const float d =
                pointForm ? DistanceToAABB(wb, center) : DistanceBetweenAABB(centerBounds, wb);
            if (d <= radius)
                hits.push_back({d, &e});
        }
        std::sort(hits.begin(), hits.end(),
                  [](const auto& l, const auto& r) { return l.first < r.first; });

        json matches = json::array();
        for (const auto& [d, e] : hits) {
            const AABB wb = boundsOrPoint(*e);
            matches.push_back({{"id", std::to_string(e->id)},
                               {"name", e->name},
                               {"distance", d},
                               {"center", Vec3Json((wb.min + wb.max) * 0.5f)}});
        }
        return JsonResult(json{{"center", Vec3Json(center)},
                               {"radius", radius},
                               {"count", matches.size()},
                               {"matches", std::move(matches)}});
    }

    if (action == "above_height" || action == "below_height") {
        if (!args.contains("height") || !args["height"].is_number())
            return Err("Provide height");
        const float height = args["height"];
        const bool above = action == "above_height";

        json matches = json::array();
        for (const Entity& e : m_Scene.Entities()) {
            const AABB wb = boundsOrPoint(e);
            // "Above" means the whole box clears the height (and vice versa) —
            // an entity straddling the plane matches neither direction.
            const bool match = above ? wb.min.y >= height : wb.max.y <= height;
            if (match)
                matches.push_back({{"id", std::to_string(e.id)},
                                   {"name", e.name},
                                   {"minY", wb.min.y},
                                   {"maxY", wb.max.y}});
        }
        return JsonResult(json{{"height", height},
                               {"direction", above ? "above" : "below"},
                               {"count", matches.size()},
                               {"matches", std::move(matches)}});
    }

    if (action == "ground_height") {
        if (!args.contains("x") || !args["x"].is_number() || !args.contains("z") ||
            !args["z"].is_number())
            return Err("Provide x and z");
        const float x = args["x"];
        const float z = args["z"];

        // Cast from just above the scene's top so stacked geometry reports its
        // highest surface, whatever the scene's vertical extent. The bump must
        // survive float absorption at large |top| (past 2^24, top + 1 == top,
        // and an on-surface origin skips the top face).
        float top = 0.0f;
        bool any = false;
        for (const Entity& e : m_Scene.Entities())
            if (e.mesh) {
                const AABB wb = WorldBoundsOf(m_Scene, e);
                if (wb.Valid()) {
                    top = any ? std::max(top, wb.max.y) : wb.max.y;
                    any = true;
                }
            }
        if (!any)
            return JsonResult(json{{"hit", false}});

        Ray ray;
        ray.origin = {x, top + std::max(1.0f, 1e-5f * std::fabs(top)), z};
        ray.direction = {0.0f, -1.0f, 0.0f};
        std::optional<RaycastHit> hit = m_Scene.Raycast(ray);
        if (!hit)
            return JsonResult(json{{"hit", false}});
        const Entity* e = m_Scene.Find(hit->entity);
        return JsonResult(json{{"hit", true},
                               {"height", hit->worldPos.y},
                               {"entityId", std::to_string(hit->entity)},
                               {"entityName", e ? e->name : ""}});
    }

    return Err("Unknown action \"" + action +
               "\" (within_radius, above_height, below_height, ground_height)");
}

// --- relational placement (#95) ---------------------------------------------------

// Move a root by a world-space delta, expressed in its parent's space so the
// hierarchy stays intact (same trick as DropToGroundSelected).
static void ApplyWorldDelta(Scene& scene, Entity& root, const vec3& worldDelta)
{
    const mat4 parentWorld = scene.WorldTransform(root.parent);
    root.transform.translation += vec3(glm::inverse(parentWorld) * vec4(worldDelta, 0.0f));
}

// Collision candidates: every meshed entity except light gizmos and the given
// exclusions (the moving subtree, and usually the anchor — contact with the
// anchor is the point of the relation).
static std::vector<AABB> ObstacleBounds(const Scene& scene,
                                        const std::unordered_set<UUID>& exclude)
{
    std::vector<AABB> out;
    for (const Entity& e : scene.Entities())
        if (e.mesh && !e.light.enabled && !exclude.count(e.id)) {
            const AABB b = WorldBoundsOf(scene, e);
            if (b.Valid())
                out.push_back(b);
        }
    return out;
}

AABB EditorApp::SubtreeWorldBounds(UUID root, std::unordered_set<UUID>* members)
{
    AABB box;
    for (UUID node : SubtreeOf(root)) {
        if (members)
            members->insert(node);
        if (Entity* e = m_Scene.Find(node); e && e->mesh) {
            const AABB b = WorldBoundsOf(m_Scene, *e);
            if (b.Valid()) {
                box.Expand(b.min);
                box.Expand(b.max);
            }
        }
    }
    return box;
}

// Resolve {"ids": [...]} / {"names": [...]} into root entities. Rejects
// duplicates and nested picks (moving an ancestor and its descendant in one
// arrangement double-moves the descendant).
static bool ResolveEntityList(Scene& scene, const json& args, std::vector<Entity*>& out,
                              std::string& error)
{
    const bool byId = args.contains("ids") && args["ids"].is_array();
    const char* key = byId ? "ids" : "names";
    if (!byId && !(args.contains("names") && args["names"].is_array())) {
        error = "Provide ids or names (array)";
        return false;
    }
    for (const json& v : args[key]) {
        if (!v.is_string()) {
            error = std::string(key) + " must be an array of strings";
            return false;
        }
        json one;
        one[byId ? "id" : "name"] = v;
        Entity* e = FindToolTarget(scene, one, error);
        if (!e)
            return false;
        out.push_back(e);
    }
    for (size_t i = 0; i < out.size(); ++i)
        for (size_t j = i + 1; j < out.size(); ++j) {
            if (out[i]->id == out[j]->id) {
                error = "Entity \"" + out[i]->name + "\" listed twice";
                return false;
            }
            if (scene.IsDescendantOf(out[i]->id, out[j]->id) ||
                scene.IsDescendantOf(out[j]->id, out[i]->id)) {
                error = "\"" + out[i]->name + "\" and \"" + out[j]->name +
                        "\" are nested — list only the roots you want moved";
                return false;
            }
        }
    return true;
}

static json BoundsJson(const AABB& b)
{
    return {{"min", Vec3Json(b.min)},
            {"max", Vec3Json(b.max)},
            {"center", Vec3Json((b.min + b.max) * 0.5f)}};
}

// Best-effort conversion of an absolute world yaw into a root's parent space:
// subtract the yaw of the parent's world forward axis. Exact for unparented
// roots and yaw-only parent rotations (mirrors ReparentKeepWorld's best-effort
// decompose); a degenerate parent basis falls back to the world yaw.
static float WorldYawToParent(const Scene& scene, UUID parent, float worldYaw)
{
    if (!parent)
        return worldYaw;
    const vec3 fwd = vec3(scene.WorldTransform(parent) * vec4(0.0f, 0.0f, 1.0f, 0.0f));
    if (std::fabs(fwd.x) + std::fabs(fwd.z) < 1e-6f)
        return worldYaw;
    return worldYaw - std::atan2(fwd.x, fwd.z);
}

// Pairwise interpenetration among ring members (the radius only guarantees
// clearance from the anchor — a crowded ring overlaps neighbor-to-neighbor).
static bool RingOverlaps(const std::vector<AABB>& memberBounds,
                         const std::vector<PlacedPose>& poses)
{
    std::vector<AABB> placed;
    placed.reserve(memberBounds.size());
    for (size_t i = 0; i < memberBounds.size(); ++i)
        placed.push_back({memberBounds[i].min + poses[i].delta,
                          memberBounds[i].max + poses[i].delta});
    for (size_t i = 0; i < placed.size(); ++i)
        for (size_t j = i + 1; j < placed.size(); ++j)
            if (OverlapAABB(placed[i], placed[j]).overlap)
                return true;
    return false;
}

ToolResult EditorApp::ToolPlaceRelative(const json& args)
{
    const std::string relation = args.value("relation", "");

    std::string error;
    Entity* anchor = FindToolTargetKeyed(m_Scene, args, "otherId", "otherName", error);
    if (!anchor)
        return Err(error);
    std::unordered_set<UUID> anchorIds;
    const AABB anchorBounds = SubtreeWorldBounds(anchor->id, &anchorIds);
    if (!anchorBounds.Valid())
        return Err("Anchor \"" + anchor->name + "\" has no bounds (no mesh in its subtree)");
    const vec3 anchorCenter = (anchorBounds.min + anchorBounds.max) * 0.5f;

    // around arranges a list of existing entities when one is given.
    if (relation == "around" &&
        ((args.contains("ids") && args["ids"].is_array()) ||
         (args.contains("names") && args["names"].is_array()))) {
        std::vector<Entity*> members;
        if (!ResolveEntityList(m_Scene, args, members, error))
            return Err(error);
        if (members.empty())
            return Err("Empty entity list");
        const float clearance = args.value("clearance", 0.0f);

        // Validate everything BEFORE the first mutation: an Err after a
        // partial apply would strand moved entities outside both the undo
        // stack and a surrounding script batch's rollback.
        std::vector<AABB> memberBounds;
        std::vector<PlacedPose> poses(members.size());
        for (size_t i = 0; i < members.size(); ++i) {
            Entity* m = members[i];
            if (m->id == anchor->id || m_Scene.IsDescendantOf(m->id, anchor->id) ||
                m_Scene.IsDescendantOf(anchor->id, m->id))
                return Err("\"" + m->name + "\" overlaps the anchor's hierarchy");
            const AABB mb = SubtreeWorldBounds(m->id, nullptr);
            if (!mb.Valid())
                return Err("Entity \"" + m->name + "\" has no bounds");
            memberBounds.push_back(mb);
            poses[i] = SolveAround(mb, anchorBounds, (int)members.size(), clearance)[i];
        }

        auto composite = std::make_unique<CompositeCommand>();
        json placed = json::array();
        for (size_t i = 0; i < members.size(); ++i) {
            Entity* m = members[i];
            Entity before = *m;
            ApplyWorldDelta(m_Scene, *m, poses[i].delta);
            m->transform.rotation.y = WorldYawToParent(m_Scene, m->parent, poses[i].yawRad);
            composite->Add(std::make_unique<EditEntityCommand>(before, *m));
            placed.push_back({{"id", std::to_string(m->id)},
                              {"name", m->name},
                              {"yawDeg", glm::degrees(poses[i].yawRad)}});
        }
        m_Commands.Push(std::move(composite));
        json j{{"relation", "around"}, {"count", placed.size()},
               {"placed", std::move(placed)}};
        if (RingOverlaps(memberBounds, poses))
            j["residualOverlap"] = true;
        return JsonResult(j);
    }

    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);
    if (e->id == anchor->id)
        return Err("Entity and anchor are the same");
    if (m_Scene.IsDescendantOf(e->id, anchor->id) || m_Scene.IsDescendantOf(anchor->id, e->id))
        return Err("Entity and anchor must not be in the same hierarchy branch");

    std::unordered_set<UUID> movingIds;
    const AABB bounds = SubtreeWorldBounds(e->id, &movingIds);
    if (!bounds.Valid())
        return Err("Entity \"" + e->name + "\" has no bounds (no mesh in its subtree)");
    const vec3 center = (bounds.min + bounds.max) * 0.5f;

    if (relation == "facing") {
        Entity before = *e;
        const float worldYaw = YawToward(center, anchorCenter);
        e->transform.rotation.y = WorldYawToParent(m_Scene, e->parent, worldYaw);
        m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));
        json j = EntityJson(m_Scene, *e);
        j["yawDeg"] = glm::degrees(worldYaw);
        return JsonResult(j);
    }

    if (relation == "around") {
        const int count = std::clamp(args.value("count", 0), 0, 64);
        if (count <= 0)
            return Err("around needs count (copies) or ids/names (existing entities)");
        if (SubtreeOf(e->id).size() > 1)
            return Err("\"" + e->name + "\" has children; duplication copies only the root — "
                       "spawn the instances first and pass them via ids/names");
        const float clearance = args.value("clearance", 0.0f);
        const auto poses = SolveAround(bounds, anchorBounds, count, clearance);

        auto composite = std::make_unique<CompositeCommand>();
        const Entity preMove = *e;
        const UUID protoParent = e->parent;
        ApplyWorldDelta(m_Scene, *e, poses[0].delta);
        e->transform.rotation.y = WorldYawToParent(m_Scene, protoParent, poses[0].yawRad);
        composite->Add(std::make_unique<EditEntityCommand>(preMove, *e));
        json placed = json::array();
        placed.push_back({{"id", std::to_string(e->id)}, {"name", e->name}});
        // NOTE: Insert can grow the entity vector — `e` is dead after the
        // first iteration; only `preMove`/`protoParent` copies are used.
        for (int k = 1; k < count; ++k) {
            Entity copy = preMove;
            copy.id = GenerateUUID();
            copy.name = preMove.name + "_" + std::to_string(k + 1);
            ApplyWorldDelta(m_Scene, copy, poses[k].delta);
            copy.transform.rotation.y = WorldYawToParent(m_Scene, protoParent, poses[k].yawRad);
            m_Scene.Insert(copy);
            composite->Add(std::make_unique<AddEntityCommand>(copy));
            placed.push_back({{"id", std::to_string(copy.id)}, {"name", copy.name}});
        }
        m_Commands.Push(std::move(composite));
        json j{{"relation", "around"}, {"count", count}, {"placed", std::move(placed)}};
        if (RingOverlaps(std::vector<AABB>((size_t)count, bounds), poses))
            j["residualOverlap"] = true;
        return JsonResult(j);
    }

    vec3 delta;
    if (relation == "on" || relation == "above") {
        const float clearance = args.value("clearance", relation == "above" ? 0.25f : 0.0f);
        delta = SolveOn(bounds, anchorBounds, clearance);
    } else if (relation == "against") {
        const float clearance = args.value("clearance", 0.0f);
        const std::string sideStr = args.value("side", "");
        int side = NearestSide(bounds, anchorBounds);
        if (sideStr == "+x") side = 0;
        else if (sideStr == "-x") side = 1;
        else if (sideStr == "+z") side = 2;
        else if (sideStr == "-z") side = 3;
        else if (!sideStr.empty())
            return Err("side must be +x, -x, +z, or -z");
        delta = SolveAgainst(bounds, anchorBounds, side, clearance);
    } else {
        return Err("Unknown relation \"" + relation +
                   "\" (on, above, against, facing, around)");
    }

    // Nudge out of third-party collisions, horizontally only — the relation's
    // vertical constraint stays authoritative (a nudge must not lift the lamp
    // off the desk).
    std::unordered_set<UUID> exclude = movingIds;
    exclude.insert(anchorIds.begin(), anchorIds.end());
    const std::vector<AABB> obstacles = ObstacleBounds(m_Scene, exclude);
    AABB placedBox{bounds.min + delta, bounds.max + delta};
    vec3 nudge = NudgeOut(placedBox, obstacles);
    nudge.y = 0.0f;
    placedBox.min += nudge;
    placedBox.max += nudge;
    bool residual = false;
    for (const AABB& o : obstacles)
        if (OverlapAABB(placedBox, o).overlap) {
            residual = true;
            break;
        }
    delta += nudge;

    Entity before = *e;
    ApplyWorldDelta(m_Scene, *e, delta);
    m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));

    json j{{"relation", relation},
           {"moved", Vec3Json(delta)},
           {"worldBounds", BoundsJson(placedBox)}};
    if (glm::length(nudge) > 0.0f)
        j["nudged"] = Vec3Json(nudge);
    if (residual)
        j["residualOverlap"] = true;
    return JsonResult(j);
}

ToolResult EditorApp::ToolSnapToSurface(const json& args)
{
    std::string error;
    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);
    std::unordered_set<UUID> movingIds;
    const AABB bounds = SubtreeWorldBounds(e->id, &movingIds);
    if (!bounds.Valid())
        return Err("Entity \"" + e->name + "\" has no bounds (no mesh in its subtree)");
    const float clearance = args.value("clearance", 0.0f);

    vec3 delta(0.0f);
    json extra = json::object(); // stays empty on the drop path; update(null) throws
    vec3 dir;
    if (!GetVec3(args, "direction", dir)) {
        // Straight down: AABB drop onto whatever is underneath (robust against
        // raycasting through gaps in the mesh; matches the editor's End key).
        const float dy = DropOffsetY(bounds, ObstacleBounds(m_Scene, movingIds));
        delta.y = dy + clearance;
    } else {
        if (glm::length(dir) < 1e-6f)
            return Err("direction must be non-zero");
        dir = glm::normalize(dir);
        const vec3 center = (bounds.min + bounds.max) * 0.5f;
        // Probe from just outside the subtree's own box — the box is convex,
        // so a ray leaving it can't hit the subtree's meshes, only the world.
        float tExit = 0.0f;
        RayIntersectsAABB(Ray{center, dir}, bounds, tExit);
        constexpr float kBump = 1e-3f;
        Ray probe{center + dir * (tExit + kBump), dir};
        std::optional<RaycastHit> hit;
        float skipped = 0.0f; // distance consumed stepping past light gizmos
        for (int guard = 0; guard < 8; ++guard) {
            hit = m_Scene.Raycast(probe);
            if (!hit)
                break;
            const Entity* h = m_Scene.Find(hit->entity);
            if (!h || !h->light.enabled)
                break; // a real surface
            // Light gizmos are display-only (the drop path's obstacle set
            // skips them too) — step just past and re-cast.
            probe.origin += dir * (hit->distance + kBump);
            skipped += hit->distance + kBump;
            hit.reset();
        }
        if (!hit)
            return Err("No surface along that direction");
        hit->distance += skipped;
        // Land the leading face: the AABB's support extent along dir, not the
        // central-ray exit — a diagonal direction leads with a corner.
        const vec3 half = (bounds.max - bounds.min) * 0.5f;
        const float support = half.x * std::fabs(dir.x) + half.y * std::fabs(dir.y) +
                              half.z * std::fabs(dir.z);
        const float centerToSurface = tExit + kBump + hit->distance;
        delta = dir * (centerToSurface - support - clearance);
        const Entity* hitEntity = m_Scene.Find(hit->entity);
        extra["surfaceEntity"] = hitEntity ? hitEntity->name : "";
        extra["surfacePoint"] = Vec3Json(hit->worldPos);
    }

    Entity before = *e;
    ApplyWorldDelta(m_Scene, *e, delta);
    m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));

    json j{{"moved", Vec3Json(delta)},
           {"worldBounds", BoundsJson({bounds.min + delta, bounds.max + delta})}};
    j.update(extra);
    return JsonResult(j);
}

ToolResult EditorApp::ToolArrangeEntities(const json& args)
{
    const std::string action = args.value("action", "");
    const std::string axisStr = args.value("axis", "");
    if (axisStr != "x" && axisStr != "y" && axisStr != "z")
        return Err("axis must be x, y, or z");
    const int axis = axisStr == "x" ? 0 : axisStr == "y" ? 1 : 2;

    std::string error;
    std::vector<Entity*> roots;
    if (!ResolveEntityList(m_Scene, args, roots, error))
        return Err(error);
    if (roots.size() < 2)
        return Err("Need at least two entities");

    std::vector<AABB> boxes;
    boxes.reserve(roots.size());
    for (Entity* r : roots) {
        AABB b = SubtreeWorldBounds(r->id, nullptr);
        if (!b.Valid()) // meshless group: degrade to its world position
            b.Expand(vec3(m_Scene.WorldTransform(r->id)[3]));
        boxes.push_back(b);
    }

    std::vector<float> deltas;
    if (action == "align") {
        const std::string modeStr = args.value("mode", "center");
        AlignMode mode = AlignMode::Center;
        if (modeStr == "min") mode = AlignMode::Min;
        else if (modeStr == "max") mode = AlignMode::Max;
        else if (modeStr != "center")
            return Err("mode must be min, center, or max");
        float target;
        if (args.contains("target") && args["target"].is_number()) {
            target = args["target"];
        } else {
            const AABB& f = boxes[0];
            target = mode == AlignMode::Min      ? f.min[axis]
                     : mode == AlignMode::Max    ? f.max[axis]
                                                 : (f.min[axis] + f.max[axis]) * 0.5f;
        }
        deltas = SolveAlign(boxes, axis, mode, target);
    } else if (action == "distribute") {
        float spacing = -1.0f; // sentinel: spread evenly
        if (args.contains("spacing")) {
            if (!args["spacing"].is_number() || (float)args["spacing"] < 0.0f)
                return Err("spacing must be a number >= 0");
            spacing = args["spacing"];
        }
        deltas = SolveDistribute(boxes, axis, spacing);
    } else {
        return Err("Unknown action \"" + action + "\"");
    }

    auto composite = std::make_unique<CompositeCommand>();
    json moved = json::array();
    for (size_t i = 0; i < roots.size(); ++i) {
        if (std::fabs(deltas[i]) < 1e-7f)
            continue;
        Entity before = *roots[i];
        vec3 wd(0.0f);
        wd[axis] = deltas[i];
        ApplyWorldDelta(m_Scene, *roots[i], wd);
        composite->Add(std::make_unique<EditEntityCommand>(before, *roots[i]));
        moved.push_back({{"id", std::to_string(roots[i]->id)},
                         {"name", roots[i]->name},
                         {"delta", deltas[i]}});
    }
    if (!composite->Empty())
        m_Commands.Push(std::move(composite));
    return JsonResult(json{{"action", action}, {"axis", axisStr},
                           {"movedCount", moved.size()}, {"moved", std::move(moved)}});
}

// --- script-only editor surface (#91) ---------------------------------------------
// These handlers back forge.* Lua bindings exclusively — no top-level MCP tool.
// Minting one tool per editor op would bloat every client's tool list; the
// script IS the escape hatch (blender-mcp's execute_blender_code precedent).

ToolResult EditorApp::ToolCameraOp(const json& args)
{
    const std::string action = args.value("action", "");
    auto pose = [this]() {
        const EditorCamera::Bookmark b = m_Camera.GetBookmark();
        return json{{"position", Vec3Json(m_Camera.Position())},
                    {"focalPoint", Vec3Json(b.focalPoint)},
                    {"distance", b.distance},
                    {"pitchDeg", glm::degrees(b.pitch)},
                    {"yawDeg", glm::degrees(b.yaw)},
                    {"fovDeg", m_Camera.FOV()},
                    {"orthographic", m_Camera.IsOrthographic()}};
    };

    if (action == "get")
        return JsonResult(pose());
    if (action == "set") {
        // Orbit-parameterized on purpose: eye position is derived, so scripts
        // set focal point / distance / angles, same as the mouse does.
        ApplyPoseArgs(args, m_Camera);
        if (args.contains("orthographic") && args["orthographic"].is_boolean())
            m_Camera.SetOrthographic(args["orthographic"]);
        return JsonResult(pose());
    }
    if (action == "store" || action == "recall") {
        const int slot = args.value("slot", 1);
        if (slot < 1 || slot > 4)
            return Err("slot must be 1-4 (the editor's F1-F4 bookmarks)");
        CameraBookmark& bm = m_Bookmarks[slot - 1];
        if (action == "store") {
            bm = {true, m_Camera.GetBookmark()};
            return JsonResult(json{{"stored", slot}});
        }
        if (!bm.set)
            return Err("Bookmark slot " + std::to_string(slot) + " is empty");
        m_Camera.ApplyBookmark(bm.value);
        return JsonResult(pose());
    }
    return Err("Unknown camera action \"" + action + "\"");
}

ToolResult EditorApp::ToolSelectOp(const json& args)
{
    const std::string action = args.value("action", "");
    auto selectionJson = [this]() {
        json ids = json::array();
        for (UUID id : m_Selection)
            ids.push_back(std::to_string(id));
        json j{{"ids", std::move(ids)}};
        if (m_Selected)
            j["primary"] = std::to_string(m_Selected);
        return j;
    };

    if (action == "get")
        return JsonResult(selectionJson());
    if (action == "clear") {
        SelectOnly(0);
        return JsonResult(selectionJson());
    }
    if (action == "select" || action == "toggle") {
        std::string error;
        Entity* e = FindToolTarget(m_Scene, args, error);
        if (!e)
            return Err(error);
        if (action == "select")
            SelectOnly(e->id);
        else
            ToggleSelection(e->id);
        return JsonResult(selectionJson());
    }
    if (action == "box") {
        auto uv = [&](const char* key, vec2& out) {
            if (!args.contains(key) || !args[key].is_array() || args[key].size() != 2 ||
                !args[key][0].is_number() || !args[key][1].is_number())
                return false;
            out = {(float)args[key][0], (float)args[key][1]};
            return true;
        };
        vec2 mn, mx;
        if (!uv("min", mn) || !uv("max", mx))
            return Err("Provide min [u,v] and max [u,v] (viewport 0-1, y down)");
        const RectUV rect{glm::min(mn, mx), glm::max(mn, mx)};
        ApplyBoxSelect(rect, args.value("additive", false));
        return JsonResult(selectionJson());
    }
    return Err("Unknown selection action \"" + action + "\"");
}

ToolResult EditorApp::ToolSceneStructure(const json& args)
{
    const std::string action = args.value("action", "");
    std::string error;

    if (action == "group") {
        std::vector<Entity*> roots;
        if (!ResolveEntityList(m_Scene, args, roots, error))
            return Err(error);
        if (roots.size() < 2)
            return Err("Group needs at least two entities");
        // Ids, not pointers: GroupSelection inserts the group entity, which
        // can grow the vector and invalidate every Entity*.
        std::vector<UUID> ids;
        for (Entity* r : roots)
            ids.push_back(r->id);
        SelectOnly(ids[0]);
        for (size_t i = 1; i < ids.size(); ++i)
            ToggleSelection(ids[i]);
        GroupSelection(); // one composite; leaves the new group selected
        Entity* g = m_Scene.Find(m_Selected);
        if (!g)
            return Err("Group failed");
        return JsonResult(EntityJson(m_Scene, *g));
    }
    if (action == "ungroup") {
        Entity* e = FindToolTarget(m_Scene, args, error);
        if (!e)
            return Err(error);
        const std::vector<UUID> children = m_Scene.ChildrenOf(e->id);
        if (children.empty())
            return Err("\"" + e->name + "\" has no children to ungroup");
        SelectOnly(e->id);
        UngroupSelected(); // one composite; deletes pure-container groups
        json freed = json::array();
        for (UUID c : children)
            freed.push_back(std::to_string(c));
        return JsonResult(json{{"ok", true}, {"children", std::move(freed)}});
    }
    if (action == "drop_to_ground") {
        std::vector<UUID> ids;
        if ((args.contains("ids") && args["ids"].is_array()) ||
            (args.contains("names") && args["names"].is_array())) {
            std::vector<Entity*> roots;
            if (!ResolveEntityList(m_Scene, args, roots, error))
                return Err(error);
            for (Entity* r : roots)
                ids.push_back(r->id);
        } else {
            Entity* e = FindToolTarget(m_Scene, args, error);
            if (!e)
                return Err(error);
            ids.push_back(e->id);
        }
        SelectOnly(ids[0]);
        for (size_t i = 1; i < ids.size(); ++i)
            ToggleSelection(ids[i]);
        DropSelectedToGround(); // one composite; sequential so drops stack
        json moved = json::array();
        for (UUID id : ids)
            if (const Entity* r = m_Scene.Find(id))
                moved.push_back(EntityJson(m_Scene, *r));
        return JsonResult(json{{"dropped", std::move(moved)}});
    }
    return Err("Unknown action \"" + action + "\"");
}

ToolResult EditorApp::ToolSnapSettings(const json& args)
{
    // Editor preference, not scene state: not undoable, persists via the
    // debounced settings save (same contract as the preferences window).
    bool changed = false;
    if (args.contains("enabled") && args["enabled"].is_boolean()) {
        m_SnapEnabled = args["enabled"];
        changed = true;
    }
    if (args.contains("translate") && args["translate"].is_number()) {
        m_SnapTranslate = std::max(0.001f, (float)args["translate"]);
        changed = true;
    }
    if (args.contains("rotateDeg") && args["rotateDeg"].is_number()) {
        m_SnapRotateDeg = std::clamp((float)args["rotateDeg"], 0.1f, 180.0f);
        changed = true;
    }
    if (args.contains("scale") && args["scale"].is_number()) {
        m_SnapScale = std::max(0.001f, (float)args["scale"]);
        changed = true;
    }
    if (changed)
        QueueSettingsSave();
    return JsonResult(json{{"enabled", m_SnapEnabled},
                           {"translate", m_SnapTranslate},
                           {"rotateDeg", m_SnapRotateDeg},
                           {"scale", m_SnapScale}});
}

ToolResult EditorApp::ToolMeshElements(const json& args)
{
    std::string error;
    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);
    if (!e->mesh)
        return Err("Entity \"" + e->name + "\" has no mesh");
    const std::string kind = args.value("kind", "face");
    if (kind != "face" && kind != "edge")
        return Err("kind must be face or edge");
    const EditMesh em = BuildEditMesh(*e->mesh);
    const mat4 world = m_Scene.WorldTransform(e->id);

    // Edges bounding given faces — the bridge from a raycast face pick
    // (triIndex / 3) to edge ops. ofFaces only makes sense for edges, so its
    // presence implies the kind rather than being silently ignored.
    if (args.contains("ofFaces") && args["ofFaces"].is_array()) {
        std::vector<uint32_t> faces;
        for (const json& f : args["ofFaces"])
            if (f.is_number_integer())
                faces.push_back((uint32_t)(int64_t)f);
        json ids = json::array();
        for (uint32_t id : EdgesOfFaces(em, faces))
            ids.push_back(id);
        return JsonResult(json{{"kind", "edge"}, {"ids", std::move(ids)}});
    }

    vec3 center;
    const bool filtered = GetVec3(args, "point", center);
    const float radius = args.value("radius", 0.0f);
    if (filtered && radius <= 0.0f)
        return Err("Provide radius > 0 with point");
    const size_t maxCount = (size_t)std::clamp(args.value("maxCount", 200), 1, 1000);

    size_t total = 0;
    std::vector<ElementInfo> list =
        kind == "face"
            ? ListFaceElements(em, world, filtered ? &center : nullptr, radius, maxCount, total)
            : ListEdgeElements(em, world, filtered ? &center : nullptr, radius, maxCount, total);

    json items = json::array();
    for (const ElementInfo& el : list) {
        json j{{"id", el.id}, {"center", Vec3Json(el.center)}};
        if (kind == "face")
            j["normal"] = Vec3Json(el.normal);
        items.push_back(std::move(j));
    }
    return JsonResult(json{{"kind", kind},
                           {"total", total},
                           {"returned", items.size()},
                           {"elements", std::move(items)}});
}

ToolResult EditorApp::ToolEditElements(const json& args)
{
    const std::string action = args.value("action", "");
    std::string error;
    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);
    if (!e->mesh)
        return Err("Entity \"" + e->name + "\" has no mesh");

    // Shared tail: fresh Mesh, welded normals, one MeshSwapCommand.
    auto swap = [&](std::vector<Vertex> verts, std::vector<uint32_t> indices, const char* what,
                    json extra) -> ToolResult {
        if (indices.empty())
            return Err(std::string(what) +
                       " produced no geometry (stale ids or degenerate selection?)");
        auto after = std::make_shared<Mesh>(std::move(verts), std::move(indices));
        RecomputeNormalsWelded(*after, MeshTopology::Build(*after));
        after->UploadVertices(); // recompute touches CPU verts only; the VBO must follow
        std::shared_ptr<Mesh> before = e->mesh;
        e->mesh = after;
        m_Commands.Push(std::make_unique<MeshSwapCommand>(e->id, before, after));
        json j = EntityJson(m_Scene, *e);
        j.update(extra);
        return JsonResult(j);
    };

    if (action == "shade") {
        // COW discipline: undo history shares the mesh pointer, so shading
        // recomputes normals on a clone and swaps — never in place. Identity
        // clone: submesh ranges carry over (same index buffer, #80).
        const bool smooth = args.value("smooth", true);
        auto after = std::make_shared<Mesh>(e->mesh->Vertices(), e->mesh->Indices(), e->mesh->Submeshes());
        if (smooth)
            RecomputeNormalsWelded(*after, MeshTopology::Build(*after));
        else
            RecomputeNormalsFlat(*after);
        after->UploadVertices(); // normals changed on the CPU side only
        std::shared_ptr<Mesh> before = e->mesh;
        e->mesh = after;
        m_Commands.Push(std::make_unique<MeshSwapCommand>(e->id, before, after));
        json j = EntityJson(m_Scene, *e);
        j["shading"] = smooth ? "smooth" : "flat";
        return JsonResult(j);
    }

    std::vector<uint32_t> ids;
    if (args.contains("ids") && args["ids"].is_array())
        for (const json& v : args["ids"])
            if (v.is_number_integer())
                ids.push_back((uint32_t)(int64_t)v);
    if (ids.empty())
        return Err("Provide ids (from mesh_elements, or a raycast's triIndex / 3 for faces)");
    const EditMesh em = BuildEditMesh(*e->mesh);

    if (action == "extrude_faces" || action == "extrude_edges") {
        if (!args.contains("distance") || !args["distance"].is_number())
            return Err("Provide distance (world units along the region normal)");
        const float distance = args["distance"];
        if (std::fabs(distance) < 1e-5f)
            return Err("distance must be non-zero (a zero extrude leaves doubled "
                       "coincident geometry)");
        // The kernels build in object space; `distance` is a world-space
        // promise. Same world->object mapping as the interactive tools
        // (ExtrudeTool / edit-mode drag): scale by the normal's world length.
        auto objectDistance = [&](const vec3& objectNormal) {
            const float worldPerLocal = std::max(
                glm::length(mat3(m_Scene.WorldTransform(e->id)) * objectNormal), 1e-6f);
            return distance / worldPerLocal;
        };
        if (action == "extrude_faces") {
            FaceExtrusion ex =
                BuildFaceExtrusion(em, e->mesh->Vertices(), e->mesh->Indices(), ids);
            const float d = objectDistance(ex.normal);
            for (uint32_t vi : ex.capVerts)
                ex.vertices[vi].position += ex.normal * d;
            return swap(std::move(ex.vertices), std::move(ex.indices), "Face extrude",
                        json{{"extruded", ids.size()}});
        }
        EdgeExtrusion ex = BuildEdgeExtrusion(em, e->mesh->Vertices(), e->mesh->Indices(), ids);
        const float d = objectDistance(ex.normal);
        for (uint32_t vi : ex.movingVerts)
            ex.vertices[vi].position += ex.normal * d;
        return swap(std::move(ex.vertices), std::move(ex.indices), "Edge extrude",
                    json{{"extruded", ids.size()}});
    }
    if (action == "subdivide_faces" || action == "subdivide_edges") {
        MeshSubdivision sub =
            action == "subdivide_faces"
                ? BuildFaceSubdivision(em, e->mesh->Vertices(), e->mesh->Indices(), ids)
                : BuildEdgeSubdivision(em, e->mesh->Vertices(), e->mesh->Indices(), ids);
        return swap(std::move(sub.vertices), std::move(sub.indices), "Subdivide",
                    json{{"newVertices", sub.newVerts.size()}});
    }
    return Err("Unknown element action \"" + action + "\"");
}

// #105: script-driven sculpting. forge.sculpt applies the interactive brush
// math (grab/inflate/smooth) at a world-space point; forge.move_verts is the
// general falloff-weighted vertex translation underneath. One sparse
// SculptStrokeCommand per call — the same undo currency as a mouse stroke.
ToolResult EditorApp::ToolSculpt(const json& args)
{
    const std::string action = args.value("action", "");
    const bool moveVerts = action == "move_verts";
    std::string error;
    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);
    if (!e->mesh)
        return Err("Entity has no mesh");

    // --- validate everything before touching the mesh (validate-then-mutate)
    const char* centerKey = moveVerts ? "point" : "center";
    vec3 worldCenter;
    if (!GetVec3(args, centerKey, worldCenter))
        return Err(std::string("Provide ") + centerKey + " as [x,y,z] (world space)");
    if (!args.contains("radius") || !args["radius"].is_number())
        return Err("Provide radius (world units)");
    const float worldRadius = args["radius"];
    if (!(worldRadius > 0.0f))
        return Err("radius must be > 0");
    const int strokes = std::clamp(args.value("strokes", 1), 1, 32);
    const bool mirror = args.value("mirror", false); // repeat across the local YZ plane
    const bool snap = args.value("snap", false);     // project center onto the surface
    const bool relax = args.value("relax", false);   // smooth pass before the brush

    std::string brush = args.value("brush", "grab");
    SculptFalloff falloff = SculptFalloff::Smooth;
    if (moveVerts) {
        const std::string f = args.value("falloff", "smooth");
        if (f == "linear")
            falloff = SculptFalloff::Linear;
        else if (f == "constant")
            falloff = SculptFalloff::Constant;
        else if (f != "smooth")
            return Err("falloff must be smooth, linear or constant");
    } else if (brush != "grab" && brush != "inflate" && brush != "smooth") {
        return Err("brush must be grab, inflate or smooth");
    }

    // Per-brush strength contract: grab/inflate move the full-weight center by
    // `strength` WORLD units (negative inflate dents); smooth's strength is a
    // 0..1 relaxation factor per stroke. move_verts takes an explicit world
    // offset vector instead.
    float strength = args.value("strength", brush == "smooth" ? 0.5f : 0.25f);
    vec3 worldOffset(0.0f);
    if (moveVerts) {
        if (!GetVec3(args, "offset", worldOffset) || glm::length(worldOffset) < 1e-6f)
            return Err("Provide a non-zero offset [x,y,z] (world units)");
    } else if (brush == "grab") {
        vec3 dir;
        if (!GetVec3(args, "direction", dir) || glm::length(dir) < 1e-6f)
            return Err("grab needs a non-zero direction [x,y,z] (world space)");
        if (std::fabs(strength) < 1e-6f)
            return Err("strength must be non-zero (world units the center moves)");
        worldOffset = glm::normalize(dir) * strength;
    } else if (brush == "inflate") {
        if (std::fabs(strength) < 1e-6f)
            return Err("strength must be non-zero (world units; negative dents inward)");
    } else { // smooth
        if (strength < 1e-6f)
            return Err("smooth strength must be > 0 (0..1 relaxation factor)");
        strength = std::min(strength, 1.0f);
    }

    // World -> object: positions through the full inverse, vectors through
    // mat3(inv); the radius (and inflate's normal-space amount) scale by the
    // inverse x-basis length — the same uniform-scale approximation the
    // interactive SculptTool uses.
    const mat4 world = m_Scene.WorldTransform(e->id);
    const mat4 inv = glm::inverse(world);
    vec3 localCenter = vec3(inv * vec4(worldCenter, 1.0f));
    const float invScale = glm::length(vec3(inv[0]));
    const float localRadius = worldRadius * invScale;
    if (!(localRadius > 0.0f) || !std::isfinite(localRadius))
        return Err("Entity world transform is degenerate (zero scale?)");
    const vec3 localOffset = mat3(inv) * worldOffset;
    const float localAmount = strength * invScale; // inflate: along object normals

    // Mirror works in OBJECT space around the local YZ plane — the same
    // convention as the interactive tool's X-mirror: the center and the
    // directional part of the brush flip in x (#110).
    vec3 mirrorCenter{-localCenter.x, localCenter.y, localCenter.z};
    const vec3 mirrorOffset{-localOffset.x, localOffset.y, localOffset.z};

    // A brush that cannot reach the mesh's AABB is a no-op: bail before the
    // copy-on-write clone, so probing far from the surface costs neither a
    // mesh copy nor a path-tracer scene re-upload. Skipped while a stroke is
    // open (bounds may lag a live drag) and when snapping — snap exists
    // precisely to rescue centers that hover off the surface.
    if (!snap && !m_Sculpt.Active() &&
        DistanceToAABB(e->mesh->Bounds(), localCenter) > localRadius &&
        (!mirror || DistanceToAABB(e->mesh->Bounds(), mirrorCenter) > localRadius)) {
        json out = EntityJson(m_Scene, *e);
        out["movedGroups"] = 0;
        out["changedVertices"] = 0;
        return JsonResult(out);
    }

    // An open interactive stroke holds absolute vertex snapshots (grab start
    // positions, the stroke-begin diff base) — in-place edits underneath it
    // would double-record into two commands and break undo. Same guard every
    // topology op uses before pulling the mesh out from under the tool.
    if (m_Sculpt.Active())
        m_Sculpt.Exit();

    // Copy-on-write (same as SculptTool::Enter): primitives are shared between
    // sibling entities AND undo snapshots hold the same shared_ptr — editing
    // in place would rewrite history. Identity clone: submesh ranges carry
    // over (same index buffer, #80).
    if (e->mesh.use_count() > 1)
        e->mesh = std::make_shared<Mesh>(e->mesh->Vertices(), e->mesh->Indices(), e->mesh->Submeshes());

    const std::vector<Vertex> before = e->mesh->Vertices();
    const MeshTopology topo = MeshTopology::Build(*e->mesh);
    auto& verts = e->mesh->MutableVertices();

    // Snap: re-center on the nearest actual vertex so a center guessed off
    // the live surface lands on it. The mirrored center is re-derived from
    // the SNAPPED primary and snapped only to its own side of the plane — an
    // unrestricted search on a mesh with no geometry across the plane would
    // wander back and land a rogue second stroke on the primary side.
    bool snapped = false;
    bool mirrorActive = mirror;
    if (snap) {
        const vec3 preSnap = localCenter;
        snapped = SnapToNearestVertex(verts, topo, localCenter);
        if (mirror) {
            const vec3 reflected{-localCenter.x, localCenter.y, localCenter.z};
            mirrorCenter = reflected;
            mirrorActive = SnapToNearestVertex(verts, topo, mirrorCenter,
                                               localCenter.x >= 0.0f ? -1 : +1);
            // A same-side snap is still unbounded: on an asymmetric mesh the
            // nearest mirror-side vertex may sit far from the reflection (a
            // stray boolean leftover). Allow the mirrored snap only as much
            // travel as the primary needed, plus a brush radius of slack —
            // beyond that there is no mirror-image surface to sculpt.
            if (mirrorActive &&
                glm::length(mirrorCenter - reflected) >
                    glm::length(localCenter - preSnap) + localRadius)
                mirrorActive = false;
        }
    }
    // Both sides resolving to the same spot (center on the plane): one stroke.
    if (mirrorActive && glm::length(mirrorCenter - localCenter) < 1e-6f)
        mirrorActive = false;

    // Folded-surface detector: if most of the region's normals point into the
    // mesh, a normal-following brush will push the wrong way (the #105 face
    // field test grew bulges from "dents" this way). Warn, still apply — the
    // caller decides. Smooth is exempt: it is the repair tool.
    std::string warning;
    if (moveVerts || brush != "smooth") {
        const AABB& b = e->mesh->Bounds();
        const vec3 interior = (b.min + b.max) * 0.5f;
        float frac = InvertedNormalFraction(verts, topo, localCenter, localRadius, interior);
        if (mirrorActive)
            frac = std::max(frac,
                            InvertedNormalFraction(verts, topo, mirrorCenter, localRadius,
                                                   interior));
        if (frac > 0.5f)
            warning = "most normals in the brush region point into the mesh (folded "
                      "surface?) — inflate/grab may push the wrong way; smooth first";
    }

    // Optional pre-relax: one smooth pass over the region before the brush.
    size_t relaxedGroups = 0;
    if (relax) {
        relaxedGroups = SculptSmooth(verts, topo, localCenter, localRadius, 0.4f);
        if (mirrorActive)
            relaxedGroups += SculptSmooth(verts, topo, mirrorCenter, localRadius, 0.4f);
    }

    size_t movedGroups = 0;
    for (int s = 0; s < strokes; ++s) {
        size_t moved = 0;
        if (moveVerts || brush == "grab") {
            // Mirrored moves go through the merged kernel: each group written
            // once, mirrored side wins on overlap — a center on the plane
            // must move geometry by `strength`, not 2x (interactive parity).
            const SculptFalloff f = moveVerts ? falloff : SculptFalloff::Smooth;
            moved = mirrorActive
                        ? SculptMoveMirrored(verts, topo, localCenter, localOffset,
                                             mirrorCenter, mirrorOffset, localRadius, f)
                        : SculptMove(verts, topo, localCenter, localRadius, localOffset, f);
        } else if (brush == "inflate") {
            // Sequential double-apply matches the interactive tool; the sum
            // below is a per-application count (overlap groups count twice).
            moved = SculptInflate(verts, topo, localCenter, localRadius, localAmount);
            if (mirrorActive)
                moved += SculptInflate(verts, topo, mirrorCenter, localRadius, localAmount);
        } else {
            moved = SculptSmooth(verts, topo, localCenter, localRadius, strength);
            if (mirrorActive)
                moved += SculptSmooth(verts, topo, mirrorCenter, localRadius, strength);
        }
        movedGroups = std::max(movedGroups, moved);
        if (moved == 0)
            break;
        // Inflate pushes along normals: refresh them between strokes so the
        // accumulation follows the evolving surface.
        if (!moveVerts && brush == "inflate" && s + 1 < strokes)
            RecomputeNormalsWelded(*e->mesh, topo);
    }

    auto annotate = [&](json& j) {
        if (snapped)
            j["snappedCenter"] = Vec3Json(vec3(world * vec4(localCenter, 1.0f)));
        if (snap && mirrorActive)
            j["snappedMirrorCenter"] = Vec3Json(vec3(world * vec4(mirrorCenter, 1.0f)));
        if (!warning.empty())
            j["warning"] = warning;
    };

    json out = EntityJson(m_Scene, *e);
    out["movedGroups"] = movedGroups;
    if (movedGroups == 0 && relaxedGroups == 0) {
        out["changedVertices"] = 0;
        annotate(out);
        return JsonResult(out); // brush missed the mesh: honest no-op, no undo entry
    }

    RecomputeNormalsWelded(*e->mesh, topo);
    e->mesh->RecomputeBounds();
    e->mesh->UploadVertices(); // VBO must follow CPU edits (#103 lesson)

    // Sparse diff, same as SculptTool::EndStroke: only changed verts go on the
    // undo stack.
    const std::vector<Vertex>& after = e->mesh->Vertices();
    std::vector<uint32_t> indices;
    std::vector<Vertex> beforeDiff, afterDiff;
    for (uint32_t i = 0; i < (uint32_t)after.size() && i < (uint32_t)before.size(); ++i) {
        if (std::memcmp(&after[i], &before[i], sizeof(Vertex)) != 0) {
            indices.push_back(i);
            beforeDiff.push_back(before[i]);
            afterDiff.push_back(after[i]);
        }
    }
    out = EntityJson(m_Scene, *e); // re-read: bounds changed above
    out["movedGroups"] = movedGroups;
    out["changedVertices"] = indices.size();
    annotate(out);
    if (!indices.empty())
        m_Commands.Push(std::make_unique<SculptStrokeCommand>(
            e->id, std::move(indices), std::move(beforeDiff), std::move(afterDiff)));
    return JsonResult(out);
}

// #147: FK posing. forge.set_pose bends one named joint (euler degrees or quat) or
// applies a canned preset, re-skinning from bind. One SetPoseCommand per call stores
// only the before/after Pose — undo never snapshots the mesh.
ToolResult EditorApp::ToolSetPose(const json& args)
{
    std::string error;
    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);
    if (!e->skeleton)
        return Err("Entity has no skeleton (import a skinned model first)");
    if (!e->mesh || !e->mesh->HasSkin())
        return Err("Entity mesh carries no skin data");
    const Skeleton& sk = *e->skeleton;

    // --- validate everything before touching the mesh (validate-then-mutate) ---
    // Build the target Pose in a local, then commit on success.
    Pose before = e->pose;
    Pose next = e->pose;
    if (next.deltas.size() != sk.JointCount())
        next.deltas.assign(sk.JointCount(), glm::quat(1, 0, 0, 0)); // lazily materialize to identity

    const std::string preset = args.value("preset", "");
    if (!preset.empty()) {
        std::optional<Pose> p = MakePresetPose(sk, preset); // nullopt = unknown preset name
        if (!p)
            return Err("Unknown preset \"" + preset + "\" (rest, t-pose, a-pose, sit)");
        next = std::move(*p);
    } else {
        const std::string joint = args.value("joint", "");
        if (joint.empty())
            return Err("Provide \"joint\" (name) with euler/quat, or a \"preset\"");
        const int j = JointIndex(sk, joint);
        if (j < 0)
            return Err("No joint named \"" + joint + "\" in this skeleton");
        glm::quat delta;
        glm::vec3 euler;
        if (GetQuat(args, "quat", delta)) {
            // ok
        } else if (GetVec3(args, "euler", euler)) {
            delta = glm::quat(glm::radians(euler)); // intrinsic XYZ; bends in the bone's local frame
        } else {
            return Err("Provide \"euler\" [x,y,z] degrees or \"quat\" [x,y,z,w]");
        }
        next.deltas[(size_t)j] = delta;
    }

    // --- mutate: COW the mesh if it is shared (sibling entity or undo snapshot), then
    // re-skin from bind. CloneSkinnedMesh rebuilds from BindVertices so we never pose
    // on top of posed geometry.
    if (e->mesh.use_count() > 1) {
        std::shared_ptr<Mesh> priv = CloneSkinnedMesh(*e->mesh);
        if (priv)
            e->mesh = std::move(priv);
    }
    e->pose = std::move(next);
    ApplyPose(*e->mesh, *e->skeleton, e->pose);
    m_Commands.Push(std::make_unique<SetPoseCommand>(e->id, std::move(before), e->pose));

    json out = EntityJson(m_Scene, *e);
    out["joint"] = preset.empty() ? args.value("joint", "") : std::string();
    out["preset"] = preset;
    out["jointCount"] = (uint64_t)sk.JointCount();
    return JsonResult(out);
}

ToolResult EditorApp::ToolExportStl(const json& args)
{
    const std::string path = args.value("path", "");
    if (path.empty())
        return Err("Provide path (.stl)");

    std::vector<UUID> ids;
    if ((args.contains("ids") && args["ids"].is_array()) ||
        (args.contains("names") && args["names"].is_array())) {
        std::vector<Entity*> roots;
        std::string error;
        if (!ResolveEntityList(m_Scene, args, roots, error))
            return Err(error);
        for (Entity* r : roots)
            for (UUID n : SubtreeOf(r->id))
                ids.push_back(n);
    } else {
        for (const Entity& en : m_Scene.Entities())
            ids.push_back(en.id);
    }

    const float scale = std::max(0.001f, args.value("scale", m_StlScale));
    const StlExportResult r = ExportStl(m_Scene, ids, path, scale);
    if (!r.ok)
        return Err("Export failed: " + r.error);
    return JsonResult(json{{"ok", true},
                           {"path", path},
                           {"triangles", r.triangles},
                           {"watertight", r.watertight},
                           {"openEdges", r.openEdges},
                           {"millimetersPerUnit", scale}});
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
        } else if (primitive == "lathe" || primitive == "sweep") {
            // Point lists arrive as [[a,b],...] json arrays; a malformed entry
            // rejects the spawn rather than silently dropping points (#111).
            auto pointOk = [](const json& p, size_t dim) {
                if (!p.is_array() || p.size() != dim)
                    return false;
                for (const json& c : p)
                    if (!c.is_number())
                        return false;
                return true;
            };
            auto readVec2s = [&](const json& arr, std::vector<vec2>& out) {
                if (!arr.is_array())
                    return false;
                for (const json& p : arr) {
                    if (!pointOk(p, 2))
                        return false;
                    out.push_back({(float)p[0], (float)p[1]});
                }
                return true;
            };
            auto readVec3s = [&](const json& arr, std::vector<vec3>& out) {
                if (!arr.is_array())
                    return false;
                for (const json& p : arr) {
                    if (!pointOk(p, 3))
                        return false;
                    out.push_back({(float)p[0], (float)p[1], (float)p[2]});
                }
                return true;
            };
            std::vector<vec2> profile;
            if (!readVec2s(params.value("profile", json()), profile))
                return Err("params.profile must be [[a,b],...] number pairs");
            if (primitive == "lathe") {
                mesh = MeshFactory::Lathe(profile, params.value("sectors", 48),
                                          params.value("closed", true));
                if (!mesh)
                    return Err("Degenerate or oversized lathe profile: need >= 2 distinct "
                               "[r,y] points, r >= 0, not all on the axis, <= 4096 points "
                               "and <= 2M projected vertices");
            } else {
                std::vector<vec3> path;
                if (!readVec3s(params.value("path", json()), path))
                    return Err("params.path must be [[x,y,z],...] number triples");
                mesh = MeshFactory::Sweep(profile, path);
                if (!mesh)
                    return Err("Degenerate or oversized sweep input: need a closed profile "
                               "with >= 3 distinct points and nonzero area (<= 4096 points), "
                               "a path with >= 2 distinct points (<= 16384), and <= 2M "
                               "projected vertices");
            }
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

    int slot = 0;
    if (args.contains("slot")) {
        if (!args["slot"].is_number_integer())
            return Err("slot must be an integer");
        slot = args["slot"].get<int>();
        if (slot < 0 || slot >= (int)MaterialSlotCount(*e))
            return Err("Slot " + std::to_string(slot) + " out of range: entity has " +
                       std::to_string(MaterialSlotCount(*e)) + " material slot(s)");
    }

    Entity before = *e;
    Material& m = MaterialForSlot(*e, (uint32_t)slot);
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
    num("subsurface", m.subsurface);
    any = GetVec3(args, "subsurfaceColor", m.subsurfaceColor) | any;
    any = GetVec3(args, "subsurfaceRadius", m.subsurfaceRadius) | any;
    if (args.contains("albedoTexture") && args["albedoTexture"].is_string()) {
        const std::string path = args["albedoTexture"];
        if (path.empty()) {
            m.albedoMap = nullptr;
            m.albedoSource.clear();
        } else {
            const std::string source = "file:" + path;
            auto tex = TextureFromSource(source, TextureChannel::Albedo);
            if (!tex) {
                *e = before; // no partial edit: earlier fields already changed
                return Err("Couldn't load texture: " + path);
            }
            m.albedoMap = tex;
            m.albedoSource = source; // survives .forge round-trips (#113)
        }
        any = true;
    }
    if (!any)
        return Err("Provide at least one material property");
    m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));
    return JsonResult(EntityJson(m_Scene, *e));
}

ToolResult EditorApp::ToolSetTexture(const json& args)
{
    std::string error;
    Entity* e = FindToolTarget(m_Scene, args, error);
    if (!e)
        return Err(error);

    int slot = 0;
    if (args.contains("materialSlot")) {
        if (!args["materialSlot"].is_number_integer())
            return Err("materialSlot must be an integer");
        slot = args["materialSlot"].get<int>();
        if (slot < 0 || slot >= (int)MaterialSlotCount(*e))
            return Err("materialSlot " + std::to_string(slot) + " out of range: entity has " +
                       std::to_string(MaterialSlotCount(*e)) + " material slot(s)");
    }

    const std::string channelName = args.value("slot", "albedo");
    if (channelName == "normal")
        return Err("Normal maps aren't supported yet (the mesh format has no tangents) — "
                   "use albedo or roughness; normal maps are a tracked follow-up");
    if (channelName != "albedo" && channelName != "roughness")
        return Err("slot must be \"albedo\" or \"roughness\"");
    const TextureChannel channel =
        channelName == "albedo" ? TextureChannel::Albedo : TextureChannel::Roughness;

    // Build the texture BEFORE touching the entity (validate-then-mutate):
    // every error path must precede the first scene mutation.
    const bool clear = args.value("clear", false);
    std::string source;
    std::shared_ptr<Texture2D> tex;
    if (!clear) {
        auto it = args.find("source");
        if (it == args.end())
            return Err("Provide source (image file path string, or a procedural recipe object "
                       "{kind:'checker'|'stripes'|'gradient'|'noise'|'wood', ...}) or clear:true");
        if (it->is_string()) {
            source = "file:" + it->get<std::string>();
        } else if (it->is_object()) {
            auto recipe = RecipeFromJsonText(it->dump());
            if (!recipe)
                return Err("Invalid procedural recipe: kind must be checker/stripes/gradient/"
                           "noise/wood (other fields clamp to their ranges)");
            // Canonical form: aliases resolved, values clamped — what persists.
            source = "proc:" + RecipeToJsonText(*recipe);
        } else {
            return Err("source must be a file path string or a recipe object");
        }
        tex = TextureFromSource(source, channel);
        if (!tex)
            return Err("Couldn't build texture from source: " + source);
    }

    Entity before = *e;
    Material& m = MaterialForSlot(*e, (uint32_t)slot);
    if (channel == TextureChannel::Albedo) {
        m.albedoMap = tex;
        m.albedoSource = source;
    } else {
        m.metallicRoughnessMap = tex;
        m.mrSource = source;
    }
    m_Commands.Push(std::make_unique<EditEntityCommand>(before, *e));

    json j = EntityJson(m_Scene, *e);
    if (tex)
        j["texture"] = {{"width", tex->Width()}, {"height", tex->Height()}, {"slot", channelName}};
    return JsonResult(std::move(j));
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
    if (action == "unwrap_uv") {
        UnwrapOptions opts;
        opts.resolution = (uint32_t)std::clamp(args.value("resolution", 1024), 256, 4096);
        // Call the kernel directly (not the UnwrapUV wrapper) so the atlas
        // stats reach the agent — the packed page can differ from the request.
        auto unwrapped =
            UnwrapUVData(e->mesh->Vertices(), e->mesh->Indices(), e->mesh->Submeshes(), opts);
        if (!unwrapped)
            return Err("Unwrap UV failed (empty or degenerate input?)");
        auto after = std::make_shared<Mesh>(std::move(unwrapped->vertices),
                                            std::move(unwrapped->indices),
                                            std::move(unwrapped->submeshes));
        std::shared_ptr<Mesh> beforeMesh = e->mesh;
        e->mesh = after;
        m_Commands.Push(std::make_unique<MeshSwapCommand>(e->id, beforeMesh, after));
        json j = EntityJson(m_Scene, *e);
        j["atlas"] = {{"width", unwrapped->atlasWidth},
                      {"height", unwrapped->atlasHeight},
                      {"charts", unwrapped->chartCount},
                      {"utilization", unwrapped->utilization}};
        return JsonResult(j);
    }

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
        } else if (!args.contains("id") && !args.contains("name")) {
            // No target at all: frame the whole scene (#93).
            const AABB box = SceneFocusBounds(m_Scene);
            if (!box.Valid())
                return Err("Nothing to frame (empty scene)");
            point = (box.min + box.max) * 0.5f;
            if (!args.contains("distance"))
                radius = std::max(glm::length(box.max - box.min) * 0.6f, 1.0f);
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
    if (action == "set_render_settings") {
        // Viewer state (#92): saved with the scene like the sun/sky sliders,
        // deliberately outside the CommandStack. Startup defaults stay in
        // Preferences; this adjusts the live session.
        if (args.contains("rayTracing") && args["rayTracing"].is_boolean())
            SetRayTracing(args["rayTracing"]);
        if (args.contains("bounces") && args["bounces"].is_number())
            m_Bounces = std::clamp((int)(double)args["bounces"], 1, 16);
        if (args.contains("rtScale") && args["rtScale"].is_number())
            m_RTScale = std::clamp((float)args["rtScale"], 0.25f, 1.0f);
        if (args.contains("denoise") && args["denoise"].is_boolean())
            m_Denoise = args["denoise"];
        if (args.contains("denoiseStrength") && args["denoiseStrength"].is_number())
            m_DenoiseStrength = std::clamp((float)args["denoiseStrength"], 0.0f, 1.0f);
        if (args.contains("aperture") && args["aperture"].is_number())
            m_Aperture = std::clamp((float)args["aperture"], 0.0f, 1.0f);
        if (args.contains("focusDist") && args["focusDist"].is_number())
            m_FocusDist = (float)args["focusDist"]; // <=0 re-derives from orbit distance
        return JsonResult(json{{"rayTracing", m_RayTracing},
                               {"bounces", m_Bounces},
                               {"rtScale", m_RTScale},
                               {"denoise", m_Denoise},
                               {"denoiseStrength", m_DenoiseStrength},
                               {"aperture", m_Aperture},
                               {"focusDist", m_FocusDist}});
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

// --- multi-view diagnostics (#93) -------------------------------------------------

ToolResult EditorApp::ToolRenderViews(const json& args)
{
    const std::string preset = args.value("preset", "4up");
    const std::string mode = args.value("mode", "clay");
    const int size = std::clamp(args.value("size", 448), 128, 768);
    const bool beauty = mode == "beauty", clay = mode == "clay", wire = mode == "wireframe",
               normals = mode == "normals", objectId = mode == "object_id",
               section = mode == "section"; // #114: clay cut open at a plane
    if (!(beauty || clay || wire || normals || objectId || section))
        return Err("Unknown mode: \"" + mode + "\"");

    // A targeted call frames AND renders only that subtree — mixing in the
    // rest of the scene puts geometry past the fitted far plane, and a hard
    // clip reads as a tear to the critic.
    AABB box;
    std::unordered_set<UUID> subtree;
    if (args.contains("id") || args.contains("name")) {
        std::string error;
        Entity* e = FindToolTarget(m_Scene, args, error);
        if (!e)
            return Err(error);
        for (UUID node : SubtreeOf(e->id)) {
            subtree.insert(node);
            if (Entity* n = m_Scene.Find(node); n && n->mesh) {
                const AABB b = TransformAABB(n->mesh->Bounds(), m_Scene.WorldTransform(node));
                box.Expand(b.min);
                box.Expand(b.max);
            }
        }
    } else {
        box = SceneFocusBounds(m_Scene);
    }
    if (!box.Valid())
        return Err("Nothing to frame (no meshes)");

    // Section cut plane (#114). Default: the z = center plane with normal -z,
    // which keeps the back half — the cut face points straight at the presets'
    // front camera, so the default call IS the wall-thickness shot.
    vec3 planeOrigin = (box.min + box.max) * 0.5f;
    vec3 planeNormal{0.0f, 0.0f, -1.0f};
    if (section && args.contains("plane")) {
        const json& plane = args["plane"];
        if (!plane.is_object())
            return Err("plane must be an object {origin:[x,y,z], normal:[x,y,z]}");
        if (plane.contains("origin") && !GetVec3(plane, "origin", planeOrigin))
            return Err("plane.origin must be [x,y,z]");
        if (plane.contains("normal") && !GetVec3(plane, "normal", planeNormal))
            return Err("plane.normal must be [x,y,z]");
        if (glm::length(planeNormal) < 1e-6f)
            return Err("plane.normal must be non-zero");
    }

    const std::vector<ViewSpec> specs = BuildViewSpecs(preset, box);
    if (specs.empty())
        return Err("Unknown preset: \"" + preset + "\"");

    // Stable object-id colors in scene order, with a legend the critic can name.
    std::unordered_map<UUID, vec3> idColors;
    json legend = json::array();
    if (objectId) {
        size_t k = 0;
        for (const Entity& e : m_Scene.Entities())
            if (e.mesh) {
                const vec3 c = IdColor(k++);
                idColors[e.id] = c;
                char hex[8];
                std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", (int)(c.r * 255.0f + 0.5f),
                              (int)(c.g * 255.0f + 0.5f), (int)(c.b * 255.0f + 0.5f));
                legend.push_back({{"id", std::to_string(e.id)}, {"name", e.name}, {"color", hex}});
            }
    }

    // Scope guard, not a lambda: tool handlers may throw (the protocol layer
    // converts to isError and the app keeps running), and leaked DebugView /
    // grid state would stick to the interactive viewport until the next
    // successful call. The destructor also drops m_DisplayTex once the shared
    // post stack has been resized under it — the recorded GL name is deleted,
    // and DrawViewport's ==0 fallback shows the raw attachment instead.
    struct RendererStateGuard {
        EditorApp& app;
        ShadingMode shading;
        DebugView debug;
        bool grid;
        bool postResized = false;
        ~RendererStateGuard()
        {
            app.m_Renderer.SetDebugView(debug);
            app.m_Renderer.SetShadingMode(shading);
            app.m_Renderer.SetEnvironment(app.m_Env.get());
            app.m_Renderer.SetGridEnabled(grid);
            app.m_Renderer.SetSectionPlane({}, {}, false); // never leak the cut into the viewport
            if (postResized)
                app.m_DisplayTex = 0;
        }
    } guard{*this, m_Renderer.GetShadingMode(), m_Renderer.GetDebugView(),
            m_Renderer.GridEnabled()};

    m_Renderer.SetGridEnabled(false); // agent views: geometry only
    m_Renderer.SetDebugView(wire      ? DebugView::Wireframe
                            : normals ? DebugView::Normals
                            : objectId ? DebugView::Unlit
                                       : DebugView::None);
    if (beauty || clay || section)
        m_Renderer.SetShadingMode(ShadingMode::PBR); // section needs the PBR path (clip + cap shader)
    m_Renderer.SetEnvironment(beauty ? m_Env.get() : nullptr); // sky only in beauty
    if (section)
        m_Renderer.SetSectionPlane(planeOrigin, planeNormal, true);

    Material clayMat;
    clayMat.albedo = {0.72f, 0.72f, 0.70f};
    clayMat.metallic = 0.0f;
    clayMat.roughness = 0.6f;

    Framebuffer fbo((uint32_t)size, (uint32_t)size, /*hdr=*/true);
    ToolResult result;
    json views = json::array();
    for (const ViewSpec& spec : specs) {
        m_Renderer.BeginScene(ViewProjFor(spec, 1.0f), spec.eye, m_Sun);
        for (const Entity& e : m_Scene.Entities()) {
            const mat4 world = m_Scene.WorldTransform(e.id);
            if ((beauty || clay || section) && e.light.enabled)
                m_Renderer.SubmitLight(vec3(world[3]), e.light.color, e.light.intensity,
                                       e.light.range);
            if (!e.mesh)
                continue;
            if (!subtree.empty() && !subtree.count(e.id))
                continue; // targeted call: render the subtree only
            if (!beauty && e.light.enabled)
                continue; // gizmo spheres read as floating-part false positives
            auto styled = [&](const Material& base) {
                Material mat = base;
                if (clay || section)
                    mat = clayMat;
                else if (wire)
                    mat.albedo = {0.75f, 0.85f, 1.0f};
                else if (objectId)
                    mat.albedo = idColors[e.id];
                if (!beauty) { // diagnostics want opaque geometry, no texture noise
                    mat.transmission = 0.0f;
                    mat.albedoMap = nullptr;
                    mat.metallicRoughnessMap = nullptr;
                }
                return mat;
            };
            // Diagnostic styles are uniform across the mesh, so only beauty needs
            // per-submesh materials (#80).
            const auto& subs = e.mesh->Submeshes();
            if (!beauty || subs.empty()) {
                m_Renderer.Submit(*e.mesh, world, styled(e.material), !e.light.enabled);
            } else {
                for (const Submesh& sm : subs)
                    m_Renderer.Submit(*e.mesh, world, styled(MaterialForSlot(e, sm.materialSlot)),
                                      !e.light.enabled, sm.firstIndex, sm.indexCount);
            }
        }
        m_Renderer.EndScene(fbo);
        fbo.Unbind();
        // Lit modes go through the post stack (tonemap); diagnostic modes are
        // already display-encoded and read straight from the HDR attachment.
        uint32_t tex = fbo.ColorAttachment();
        if (beauty || clay || section) {
            tex = m_Post.Process(fbo.ColorAttachment(), (uint32_t)size, (uint32_t)size);
            guard.postResized = true; // m_DisplayTex now names a deleted texture
        }
        std::string b64 = TextureToPngBase64(tex, size);
        if (b64.empty())
            return Err("View readback failed");
        result.content.push_back({{"type", "text"}, {"text", "view: " + spec.label}});
        result.content.push_back(
            {{"type", "image"}, {"data", std::move(b64)}, {"mimeType", "image/png"}});
        views.push_back(spec.label);
    }

    json info{{"preset", preset}, {"mode", mode}, {"views", views}};
    if (objectId)
        info["legend"] = legend;
    if (section)
        info["plane"] = {{"origin", Vec3Json(planeOrigin)},
                         {"normal", Vec3Json(planeNormal)},
                         {"note", "amber = cut surface; negative side removed"}};
    result.content.push_back({{"type", "text"}, {"text", info.dump(2)}});
    return result;
}

// --- scripting (#78) ------------------------------------------------------------

ToolResult EditorApp::ToolExecuteScript(const json& args)
{
    if (!args.contains("source") || !args["source"].is_string())
        return Err("Provide source (Lua)");
    const std::string source = args["source"];

    // Every command a binding pushes collects into one composite: the whole
    // script is a single undo entry, and a failed script rolls back atomically
    // (selection included).
    const UUID selectedBefore = m_Selected;
    const std::vector<UUID> selectionBefore = m_Selection;
    m_Commands.BeginBatch();

    // Each forge.* function re-enters an existing tool handler with the same
    // JSON contract; an isError result becomes a Lua error (aborting the
    // script), a JSON result comes back as a table.
    auto call = [this](ToolResult (EditorApp::*handler)(const json&), const char* action) {
        return [this, handler, action](const json& fnArgs) -> json {
            json a = fnArgs;
            if (action)
                a["action"] = action;
            // Same front-door refusal as MCP dispatch: a Lua 0/0 or math.huge
            // arrives as a well-typed number and survives every downstream
            // range check. Throwing before the handler runs keeps the failed
            // script's rollback trivially correct — nothing mutated (#104).
            if (const std::string bad = NonFiniteArgPath(a); !bad.empty())
                throw std::runtime_error("argument '" + bad + "' is NaN or infinite");
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
        add("check_overlap", call(&EditorApp::ToolCheckOverlap, nullptr));
        add("query_spatial", call(&EditorApp::ToolQuerySpatial, nullptr));
        add("measure", call(&EditorApp::ToolMeasure, nullptr)); // #114
        add("compare_silhouette", call(&EditorApp::ToolCompareSilhouette, nullptr)); // #114: numbers only
        add("analyze_reference", call(&EditorApp::ToolAnalyzeReference, nullptr)); // #135: numbers only

        add("spawn", call(&EditorApp::ToolManageEntity, "spawn"));
        add("delete", call(&EditorApp::ToolManageEntity, "delete"));
        add("duplicate", call(&EditorApp::ToolManageEntity, "duplicate"));
        add("rename", call(&EditorApp::ToolManageEntity, "rename"));
        add("set_transform", call(&EditorApp::ToolManageEntity, "set_transform"));
        add("set_parent", call(&EditorApp::ToolManageEntity, "set_parent"));

        add("set_material", call(&EditorApp::ToolManageMaterial, nullptr));
        add("set_texture", call(&EditorApp::ToolSetTexture, nullptr));
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
        add("unwrap_uv", call(&EditorApp::ToolEditMesh, "unwrap_uv"));

        add("place_relative", call(&EditorApp::ToolPlaceRelative, nullptr));
        add("snap_to_surface", call(&EditorApp::ToolSnapToSurface, nullptr));
        add("align", call(&EditorApp::ToolArrangeEntities, "align"));
        add("distribute", call(&EditorApp::ToolArrangeEntities, "distribute"));

        add("look_at", call(&EditorApp::ToolManageScene, "look_at"));
        add("set_render_settings", call(&EditorApp::ToolManageScene, "set_render_settings"));

        // #91: extended editor surface — script-only, no top-level MCP tools.
        add("camera", call(&EditorApp::ToolCameraOp, "get"));
        add("set_camera", call(&EditorApp::ToolCameraOp, "set"));
        add("store_view", call(&EditorApp::ToolCameraOp, "store"));
        add("recall_view", call(&EditorApp::ToolCameraOp, "recall"));

        add("select", call(&EditorApp::ToolSelectOp, "select"));
        add("toggle_select", call(&EditorApp::ToolSelectOp, "toggle"));
        add("clear_selection", call(&EditorApp::ToolSelectOp, "clear"));
        add("get_selection", call(&EditorApp::ToolSelectOp, "get"));
        add("box_select", call(&EditorApp::ToolSelectOp, "box"));

        add("group", call(&EditorApp::ToolSceneStructure, "group"));
        add("ungroup", call(&EditorApp::ToolSceneStructure, "ungroup"));
        add("drop_to_ground", call(&EditorApp::ToolSceneStructure, "drop_to_ground"));
        add("snap_settings", call(&EditorApp::ToolSnapSettings, nullptr));

        add("mesh_elements", call(&EditorApp::ToolMeshElements, nullptr));
        add("sculpt", call(&EditorApp::ToolSculpt, "sculpt"));
        add("move_verts", call(&EditorApp::ToolSculpt, "move_verts"));
        add("set_pose", call(&EditorApp::ToolSetPose, nullptr));
        add("extrude_faces", call(&EditorApp::ToolEditElements, "extrude_faces"));
        add("extrude_edges", call(&EditorApp::ToolEditElements, "extrude_edges"));
        add("subdivide_faces", call(&EditorApp::ToolEditElements, "subdivide_faces"));
        add("subdivide_edges", call(&EditorApp::ToolEditElements, "subdivide_edges"));
        add("shade", call(&EditorApp::ToolEditElements, "shade"));

        add("export_stl", call(&EditorApp::ToolExportStl, nullptr));
    });

    std::unique_ptr<CompositeCommand> batch = m_Commands.EndBatch();
    if (!run.ok) {
        if (batch && !batch->Empty())
            batch->Undo(m_Scene); // roll back the partial build
        // The undo restores every entity the script touched, so the pre-script
        // selection is valid again.
        m_Selected = selectedBefore;
        m_Selection = selectionBefore;
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

    m_PathTracer.Dispatch(job.viewProj, job.camPos, m_Sun, job.bounces, m_FrameLights,
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
