#include "EditTool.h"

#include "EditorCamera.h"

#include <forge/core/Log.h>

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace forge {

namespace {
// Crease threshold for which edges read as real wireframe lines; flat-face
// triangulation diagonals fall below it and stay hidden (see EditMesh).
constexpr float kCreaseThreshold = 0.5236f; // 30 degrees in radians

// Pick tolerances in viewport pixels (vert dots draw at 3-5px, so a slightly
// larger grab radius feels right; edges are thin so allow a bit of slop).
constexpr float kVertPickPx = 9.0f;
constexpr float kEdgePickPx = 7.0f;

// High-contrast palette: meshes are often warm/orange, so verts use a
// white fill with a dark ring (legible on any albedo or against the grid),
// edges use cyan, and the emphasised element switches to a saturated colour.
constexpr ImU32 kEdge = IM_COL32(50, 190, 255, 220);    // cyan wireframe
constexpr ImU32 kEdgeHot = IM_COL32(130, 220, 255, 255); // emphasised in Edge mode
constexpr ImU32 kVertFill = IM_COL32(245, 246, 250, 255);
constexpr ImU32 kVertHot = IM_COL32(90, 150, 255, 255);  // emphasised in Vertex mode
constexpr ImU32 kOutline = IM_COL32(15, 16, 22, 255);    // dark ring for contrast
constexpr ImU32 kFaceHot = IM_COL32(255, 210, 40, 255);  // amber face dots
constexpr ImU32 kSelected = IM_COL32(240, 148, 56, 255); // ember — matches the theme accent
constexpr ImU32 kSelectedFill = IM_COL32(240, 148, 56, 70);

// Object-space point -> viewport pixel. False for points behind the camera
// (w <= 0), which must be culled before the perspective divide.
bool Project(const mat4& mvp, const vec3& p, const vec2& vpPos, const vec2& vpSize, vec2& out)
{
    vec4 c = mvp * vec4(p, 1.0f);
    if (c.w <= 0.0f)
        return false;
    c /= c.w;
    out = {vpPos.x + (c.x * 0.5f + 0.5f) * vpSize.x, vpPos.y + (1.0f - (c.y * 0.5f + 0.5f)) * vpSize.y};
    return true;
}

bool Contains(const std::vector<uint32_t>& ids, uint32_t id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

ImVec2 Im(const vec2& v) { return ImVec2(v.x, v.y); }
} // namespace

void EditTool::SetMode(Element mode)
{
    if (mode == m_Mode)
        return;
    m_Mode = mode;
    m_Selected.clear(); // element ids index a different vector now
}

void EditTool::Enter(Scene& scene, UUID entity)
{
    Entity* e = scene.Find(entity);
    if (!e || !e->mesh)
        return;
    // Copy-on-write: primitives are shared between entities AND undo snapshots
    // hold the same shared_ptr — editing in place would corrupt siblings/history.
    // Identity clone: submesh ranges carry over (same index buffer, #80).
    if (e->mesh.use_count() > 1)
        e->mesh = std::make_shared<Mesh>(e->mesh->Vertices(), e->mesh->Indices(), e->mesh->Submeshes());

    m_EditMesh = BuildEditMesh(*e->mesh);
    m_Topology = MeshTopology::Build(*e->mesh); // welded-normal recompute after edits
    m_Target = entity;
    m_MeshAtEnter = e->mesh.get();
    m_MeshVersionSeen = e->mesh->Version();
    m_Active = true;
    m_Selected.clear();
    FORGE_INFO("Edit mode: %s (%zu verts, %zu edges, %zu faces)", e->name.c_str(),
               m_EditMesh.vertices.size(), m_EditMesh.edges.size(), m_EditMesh.faces.size());
}

void EditTool::Exit()
{
    if (m_Active)
        FORGE_INFO("Edit mode: exit");
    m_Active = false;
    m_Target = 0;
    m_MeshAtEnter = nullptr;
    m_EditMesh = {};
    m_Topology = {};
    m_Selected.clear();
    m_DragVerts.clear();
    m_DragStartPos.clear();
    m_MeshBefore.clear();
    m_Extruding = false;
    m_ExtrudeOriginal.reset();
    m_ExtrudeMoving.clear();
    m_ExtrudeBase.clear();
    m_ExtrudeNewEdges.clear();
}

void EditTool::DrawOverlay(Scene& scene, const EditorCamera& camera, const vec2& viewportPos,
                           const vec2& viewportSize)
{
    if (!m_Active)
        return;
    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh) {
        Exit(); // target deleted under us
        return;
    }
    if (e->mesh.get() != m_MeshAtEnter) {
        Exit(); // mesh swapped (topology op / undo) — the snapshot is stale
        return;
    }
    // An in-place edit we didn't make (undo/redo of a transform) bumps Version
    // without swapping the pointer — resync the snapshot so the overlay tracks.
    if (e->mesh->Version() != m_MeshVersionSeen) {
        m_EditMesh = BuildEditMesh(*e->mesh);
        m_MeshVersionSeen = e->mesh->Version();
    }

    mat4 mvp = camera.ViewProjection() * scene.WorldTransform(m_Target); // object -> clip
    auto project = [&](const vec3& p, vec2& out) { return Project(mvp, p, viewportPos, viewportSize, out); };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool vertMode = m_Mode == Element::Vertex;
    const bool edgeMode = m_Mode == Element::Edge;
    const bool faceMode = m_Mode == Element::Face;

    // Selected faces first, as a translucent fill under the wireframe.
    if (faceMode) {
        for (uint32_t f : m_Selected) {
            const EditFace& face = m_EditMesh.faces[f];
            vec2 a, b, c;
            if (project(m_EditMesh.vertices[face.v[0]].position, a) &&
                project(m_EditMesh.vertices[face.v[1]].position, b) &&
                project(m_EditMesh.vertices[face.v[2]].position, c))
                dl->AddTriangleFilled(Im(a), Im(b), Im(c), kSelectedFill);
        }
    }

    // Wireframe: crease edges only, so flat-face diagonals stay hidden. Edges
    // are emphasised (brighter + thicker) in Edge mode, ember when selected.
    for (uint32_t i = 0; i < m_EditMesh.edges.size(); ++i) {
        const EditEdge& edge = m_EditMesh.edges[i];
        if (!IsCreaseEdge(edge, kCreaseThreshold))
            continue;
        vec2 a, b;
        if (!project(m_EditMesh.vertices[edge.v0].position, a) ||
            !project(m_EditMesh.vertices[edge.v1].position, b))
            continue;
        bool sel = edgeMode && Contains(m_Selected, i);
        ImU32 col = sel ? kSelected : (edgeMode ? kEdgeHot : kEdge);
        dl->AddLine(Im(a), Im(b), col, sel ? 3.0f : (edgeMode ? 2.4f : 1.5f));
    }

    // Vertices: always shown so the cage is legible, emphasised in Vertex mode,
    // ember when selected. White fill + dark ring stays visible on any albedo.
    for (uint32_t i = 0; i < m_EditMesh.vertices.size(); ++i) {
        vec2 s;
        if (!project(m_EditMesh.vertices[i].position, s))
            continue;
        bool sel = vertMode && Contains(m_Selected, i);
        float r = sel ? 6.0f : (vertMode ? 5.0f : 3.0f);
        dl->AddCircleFilled(Im(s), r, sel ? kSelected : (vertMode ? kVertHot : kVertFill));
        dl->AddCircle(Im(s), r, kOutline, 0, 1.5f);
    }

    // Face centroids: only marked in Face mode (where faces are the target).
    if (faceMode) {
        for (uint32_t i = 0; i < m_EditMesh.faces.size(); ++i) {
            vec2 s;
            if (!project(m_EditMesh.faces[i].centroid, s))
                continue;
            dl->AddCircleFilled(Im(s), 4.0f, Contains(m_Selected, i) ? kSelected : kFaceHot);
            dl->AddCircle(Im(s), 4.0f, kOutline, 0, 1.5f);
        }
    }
}

void EditTool::Pick(Scene& scene, const EditorCamera& camera, const vec2& viewportPos,
                    const vec2& viewportSize, const vec2& cursorPx, bool additive)
{
    if (!m_Active)
        return;
    mat4 mvp = camera.ViewProjection() * scene.WorldTransform(m_Target);
    auto project = [&](const vec3& p, vec2& out) { return Project(mvp, p, viewportPos, viewportSize, out); };

    bool found = false;
    uint32_t best = 0;
    float bestScore = 0.0f; // distance (vert/edge) or centroid distance (face)

    if (m_Mode == Element::Vertex) {
        for (uint32_t i = 0; i < m_EditMesh.vertices.size(); ++i) {
            vec2 s;
            if (!project(m_EditMesh.vertices[i].position, s))
                continue;
            float d = glm::length(cursorPx - s);
            if (d <= kVertPickPx && (!found || d < bestScore)) {
                found = true;
                best = i;
                bestScore = d;
            }
        }
    } else if (m_Mode == Element::Edge) {
        for (uint32_t i = 0; i < m_EditMesh.edges.size(); ++i) {
            const EditEdge& edge = m_EditMesh.edges[i];
            if (!IsCreaseEdge(edge, kCreaseThreshold))
                continue;
            vec2 a, b;
            if (!project(m_EditMesh.vertices[edge.v0].position, a) ||
                !project(m_EditMesh.vertices[edge.v1].position, b))
                continue;
            float d = DistancePointSegment2D(cursorPx, a, b);
            if (d <= kEdgePickPx && (!found || d < bestScore)) {
                found = true;
                best = i;
                bestScore = d;
            }
        }
    } else { // Face
        for (uint32_t i = 0; i < m_EditMesh.faces.size(); ++i) {
            const EditFace& face = m_EditMesh.faces[i];
            vec2 a, b, c, ctr;
            if (!project(m_EditMesh.vertices[face.v[0]].position, a) ||
                !project(m_EditMesh.vertices[face.v[1]].position, b) ||
                !project(m_EditMesh.vertices[face.v[2]].position, c))
                continue;
            if (!PointInTriangle2D(cursorPx, a, b, c))
                continue;
            // Disambiguate overlapping hits by nearest centroid (rough depth proxy).
            project(m_EditMesh.faces[i].centroid, ctr);
            float d = glm::length(cursorPx - ctr);
            if (!found || d < bestScore) {
                found = true;
                best = i;
                bestScore = d;
            }
        }
    }

    if (!found) {
        if (!additive)
            m_Selected.clear();
        return;
    }
    if (additive) {
        auto it = std::find(m_Selected.begin(), m_Selected.end(), best);
        if (it != m_Selected.end())
            m_Selected.erase(it);
        else
            m_Selected.push_back(best);
    } else {
        m_Selected.assign(1, best);
    }
}

void EditTool::BoxPick(Scene& scene, const EditorCamera& camera, const vec2& viewportPos,
                       const vec2& viewportSize, const vec2& rectMin, const vec2& rectMax, bool additive)
{
    if (!m_Active)
        return;
    if (!additive)
        m_Selected.clear();
    mat4 mvp = camera.ViewProjection() * scene.WorldTransform(m_Target);
    auto project = [&](const vec3& p, vec2& out) { return Project(mvp, p, viewportPos, viewportSize, out); };

    auto add = [&](uint32_t id) {
        if (!Contains(m_Selected, id))
            m_Selected.push_back(id);
    };

    if (m_Mode == Element::Vertex) {
        for (uint32_t i = 0; i < m_EditMesh.vertices.size(); ++i) {
            vec2 s;
            if (project(m_EditMesh.vertices[i].position, s) && PointInRect2D(s, rectMin, rectMax))
                add(i);
        }
    } else if (m_Mode == Element::Edge) {
        for (uint32_t i = 0; i < m_EditMesh.edges.size(); ++i) {
            const EditEdge& edge = m_EditMesh.edges[i];
            if (!IsCreaseEdge(edge, kCreaseThreshold))
                continue;
            vec2 a, b;
            if (project(m_EditMesh.vertices[edge.v0].position, a) &&
                project(m_EditMesh.vertices[edge.v1].position, b) &&
                PointInRect2D((a + b) * 0.5f, rectMin, rectMax))
                add(i);
        }
    } else { // Face: select when the centroid falls in the rect
        for (uint32_t i = 0; i < m_EditMesh.faces.size(); ++i) {
            vec2 s;
            if (project(m_EditMesh.faces[i].centroid, s) && PointInRect2D(s, rectMin, rectMax))
                add(i);
        }
    }
}

vec3 EditTool::SelectionCentroidObject() const
{
    return SelectionCentroid(m_EditMesh, ResolveVertexSet(m_EditMesh, m_Mode, m_Selected));
}

void EditTool::BeginTransform(Scene& scene)
{
    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh)
        return;
    m_DragVerts = ResolveVertexSet(m_EditMesh, m_Mode, m_Selected);
    m_DragStartPos.clear();
    m_DragStartPos.reserve(m_DragVerts.size());
    for (uint32_t v : m_DragVerts)
        m_DragStartPos.push_back(m_EditMesh.vertices[v].position);
    m_MeshBefore = e->mesh->Vertices(); // full snapshot for the sparse undo diff
}

void EditTool::ApplyTransform(Scene& scene, const mat4& objectXform)
{
    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh || m_DragVerts.empty())
        return;
    // Bake into the mesh (all welded raw verts), then refresh the overlay's
    // representative positions so the dots/edges track the gizmo live.
    ApplyVertexTransform(e->mesh->MutableVertices(), m_EditMesh, m_DragVerts, m_DragStartPos, objectXform);
    for (size_t i = 0; i < m_DragVerts.size(); ++i)
        m_EditMesh.vertices[m_DragVerts[i]].position = vec3(objectXform * vec4(m_DragStartPos[i], 1.0f));
    RecomputeNormalsWelded(*e->mesh, m_Topology);
    e->mesh->RecomputeBounds();
    e->mesh->UploadVertices();
    m_MeshVersionSeen = e->mesh->Version(); // our own edit — don't trigger a resync
}

std::unique_ptr<Command> EditTool::EndTransform(Scene& scene)
{
    Entity* e = scene.Find(m_Target);
    m_DragVerts.clear();
    m_DragStartPos.clear();
    if (!e || !e->mesh || m_MeshBefore.empty()) {
        m_MeshBefore.clear();
        return nullptr;
    }
    // Sparse diff vs the drag-start snapshot — only changed verts hit the stack.
    const auto& after = e->mesh->Vertices();
    std::vector<uint32_t> indices;
    std::vector<Vertex> before, now;
    for (uint32_t i = 0; i < (uint32_t)after.size() && i < (uint32_t)m_MeshBefore.size(); ++i) {
        if (std::memcmp(&after[i], &m_MeshBefore[i], sizeof(Vertex)) != 0) {
            indices.push_back(i);
            before.push_back(m_MeshBefore[i]);
            now.push_back(after[i]);
        }
    }
    m_MeshBefore.clear();
    // ApplyTransform moved the overlay's vertex positions live but left per-face
    // centroids and per-edge dihedrals stale; rebuild so picking/overlay use
    // fresh metadata. Version is unchanged (no mesh swap), so DrawOverlay's
    // watch won't redo this.
    m_EditMesh = BuildEditMesh(*e->mesh);
    if (indices.empty())
        return nullptr;
    return std::make_unique<SculptStrokeCommand>(m_Target, std::move(indices), std::move(before),
                                                 std::move(now));
}

// --- face extrude (T3, #63) --------------------------------------------------

namespace {
// Re-adopt a mesh we swapped in ourselves: refresh the staleness guard, snapshot
// and topology so DrawOverlay tracks the new geometry instead of self-exiting.
void Readopt(EditMesh& edit, MeshTopology& topo, Mesh*& guard, uint64_t& versionSeen, Mesh& mesh)
{
    guard = &mesh;
    edit = BuildEditMesh(mesh);
    topo = MeshTopology::Build(mesh);
    versionSeen = mesh.Version();
}

// After the rebuild, map each new top-edge vertex pair (raw indices into the
// committed mesh) back to its EditEdge id, so the freshly pulled edges stay
// selected for chaining. Edge ids aren't stable across a topology change, so
// this re-derives them by weld group.
std::vector<uint32_t> RemapNewEdges(const EditMesh& em,
                                    const std::vector<std::pair<uint32_t, uint32_t>>& newEdges)
{
    size_t maxRaw = 0;
    for (const EditVertex& v : em.vertices)
        for (uint32_t r : v.rawVerts)
            maxRaw = std::max(maxRaw, (size_t)r);
    std::vector<uint32_t> rawToGroup(maxRaw + 1, UINT32_MAX);
    for (uint32_t g = 0; g < em.vertices.size(); ++g)
        for (uint32_t r : em.vertices[g].rawVerts)
            rawToGroup[r] = g;

    std::vector<uint32_t> sel;
    for (auto [ra, rb] : newEdges) {
        if (ra >= rawToGroup.size() || rb >= rawToGroup.size())
            continue;
        uint32_t ga = rawToGroup[ra], gb = rawToGroup[rb];
        if (ga == UINT32_MAX || gb == UINT32_MAX)
            continue;
        const EditEdge* edge = FindEdge(em, ga, gb);
        if (!edge)
            continue;
        uint32_t id = (uint32_t)(edge - em.edges.data());
        if (std::find(sel.begin(), sel.end(), id) == sel.end())
            sel.push_back(id);
    }
    return sel;
}
} // namespace

bool EditTool::BeginExtrude(Scene& scene)
{
    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh || !CanExtrude())
        return false;

    // Build the zero-offset geometry for the active element type. Both kernels
    // return a moving-vert set + slide direction; the rest of the drag is shared.
    std::vector<Vertex> verts;
    std::vector<uint32_t> indices, moving;
    vec3 normal;
    m_ExtrudeNewEdges.clear();
    if (m_Mode == Element::Face) {
        FaceExtrusion ex = BuildFaceExtrusion(m_EditMesh, e->mesh->Vertices(), e->mesh->Indices(), m_Selected);
        if (ex.indices.empty() || ex.capVerts.empty())
            return false; // empty/stale selection or degenerate region normal
        verts = std::move(ex.vertices);
        indices = std::move(ex.indices);
        moving = std::move(ex.capVerts);
        normal = ex.normal;
    } else { // Edge
        EdgeExtrusion ex = BuildEdgeExtrusion(m_EditMesh, e->mesh->Vertices(), e->mesh->Indices(), m_Selected);
        if (ex.indices.empty() || ex.movingVerts.empty())
            return false;
        verts = std::move(ex.vertices);
        indices = std::move(ex.indices);
        moving = std::move(ex.movingVerts);
        normal = ex.normal;
        m_ExtrudeNewEdges = std::move(ex.newEdges); // remapped to edge ids on commit
    }

    m_ExtrudeOriginal = e->mesh;
    m_ExtrudeNormal = normal;
    m_ExtrudeMoving = std::move(moving);
    m_ExtrudeBase.clear();
    m_ExtrudeBase.reserve(m_ExtrudeMoving.size());
    vec3 anchor(0.0f);
    for (uint32_t v : m_ExtrudeMoving) {
        m_ExtrudeBase.push_back(verts[v].position);
        anchor += verts[v].position;
    }
    m_ExtrudeAnchor = anchor / (float)m_ExtrudeMoving.size();

    // Swap in the zero-offset geometry and re-adopt it. A face cap keeps its
    // triangle ids so m_Selected still points at it; a new edge's id isn't stable,
    // so EndExtrude remaps the selection from m_ExtrudeNewEdges.
    e->mesh = std::make_shared<Mesh>(std::move(verts), std::move(indices));
    Readopt(m_EditMesh, m_Topology, m_MeshAtEnter, m_MeshVersionSeen, *e->mesh);
    m_ExtrudeLastOffset = 0.0f;
    m_Extruding = true;
    return true;
}

void EditTool::UpdateExtrude(Scene& scene, float offset)
{
    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh || !m_Extruding)
        return;
    auto& verts = e->mesh->MutableVertices();
    for (size_t i = 0; i < m_ExtrudeMoving.size(); ++i)
        if (m_ExtrudeMoving[i] < verts.size())
            verts[m_ExtrudeMoving[i]].position = m_ExtrudeBase[i] + m_ExtrudeNormal * offset;
    // Once the cap clears the floor it welds into its own groups, so rebuild
    // topology + the overlay snapshot from the live positions each frame.
    m_Topology = MeshTopology::Build(*e->mesh);
    RecomputeNormalsWelded(*e->mesh, m_Topology);
    e->mesh->RecomputeBounds();
    e->mesh->UploadVertices();
    m_EditMesh = BuildEditMesh(*e->mesh);
    m_MeshVersionSeen = e->mesh->Version(); // our own edit — don't trigger a resync
    m_ExtrudeLastOffset = offset;
}

std::unique_ptr<Command> EditTool::EndExtrude(Scene& scene)
{
    if (!m_Extruding)
        return nullptr;
    m_Extruding = false;
    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh) {
        m_ExtrudeOriginal.reset();
        return nullptr;
    }

    float minOffset = 1e-4f * glm::length(m_ExtrudeOriginal->Bounds().max - m_ExtrudeOriginal->Bounds().min);
    if (std::abs(m_ExtrudeLastOffset) < minOffset) {
        e->mesh = m_ExtrudeOriginal; // negligible drag: restore, no undo entry
        Readopt(m_EditMesh, m_Topology, m_MeshAtEnter, m_MeshVersionSeen, *e->mesh);
        m_ExtrudeNewEdges.clear(); // original mesh restored — existing selection stays valid
        m_ExtrudeOriginal.reset();
        return nullptr;
    }

    // Fresh mesh so bounds/normals match the committed geometry; one undo step.
    // Same index buffer as the WIP mesh, so its submesh table (if any) carries over.
    auto finalMesh = std::make_shared<Mesh>(e->mesh->Vertices(), e->mesh->Indices(), e->mesh->Submeshes());
    MeshTopology topo = MeshTopology::Build(*finalMesh);
    RecomputeNormalsWelded(*finalMesh, topo);
    finalMesh->RecomputeBounds();
    finalMesh->UploadVertices();
    e->mesh = finalMesh;
    Readopt(m_EditMesh, m_Topology, m_MeshAtEnter, m_MeshVersionSeen, *e->mesh);

    // Edge extrude: face/tri ids are stable but edge ids aren't, so reselect the
    // new edges by weld group. (Face extrude leaves m_ExtrudeNewEdges empty.)
    if (!m_ExtrudeNewEdges.empty()) {
        std::vector<uint32_t> sel = RemapNewEdges(m_EditMesh, m_ExtrudeNewEdges);
        if (!sel.empty())
            m_Selected = std::move(sel);
        m_ExtrudeNewEdges.clear();
    }

    auto original = m_ExtrudeOriginal;
    m_ExtrudeOriginal.reset();
    return std::make_unique<MeshSwapCommand>(m_Target, original, finalMesh);
}

void EditTool::CancelExtrude(Scene& scene)
{
    if (!m_Extruding)
        return;
    m_Extruding = false;
    Entity* e = scene.Find(m_Target);
    if (e && e->mesh && m_ExtrudeOriginal) {
        e->mesh = m_ExtrudeOriginal;
        Readopt(m_EditMesh, m_Topology, m_MeshAtEnter, m_MeshVersionSeen, *e->mesh);
    }
    m_ExtrudeNewEdges.clear();
    m_ExtrudeOriginal.reset();
}

// --- subdivide (T5, #62) -----------------------------------------------------

namespace {
// Map raw vertex indices (from a kernel result) to their EditEdge-rebuilt weld
// groups, deduped — the selection ids for the inserted midpoint vertices.
std::vector<uint32_t> RemapNewVerts(const EditMesh& em, const std::vector<uint32_t>& rawVerts)
{
    size_t maxRaw = 0;
    for (const EditVertex& v : em.vertices)
        for (uint32_t r : v.rawVerts)
            maxRaw = std::max(maxRaw, (size_t)r);
    std::vector<uint32_t> rawToGroup(maxRaw + 1, UINT32_MAX);
    for (uint32_t g = 0; g < em.vertices.size(); ++g)
        for (uint32_t r : em.vertices[g].rawVerts)
            rawToGroup[r] = g;

    std::vector<uint32_t> sel;
    for (uint32_t raw : rawVerts) {
        if (raw >= rawToGroup.size())
            continue;
        uint32_t g = rawToGroup[raw];
        if (g != UINT32_MAX && std::find(sel.begin(), sel.end(), g) == sel.end())
            sel.push_back(g);
    }
    return sel;
}
} // namespace

std::unique_ptr<Command> EditTool::Subdivide(Scene& scene)
{
    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh || !CanSubdivide())
        return nullptr;

    MeshSubdivision sd =
        m_Mode == Element::Face
            ? BuildFaceSubdivision(m_EditMesh, e->mesh->Vertices(), e->mesh->Indices(), m_Selected)
            : BuildEdgeSubdivision(m_EditMesh, e->mesh->Vertices(), e->mesh->Indices(), m_Selected);
    if (sd.indices.empty() || sd.newVerts.empty())
        return nullptr; // empty/stale selection

    auto original = e->mesh;
    auto finalMesh = std::make_shared<Mesh>(std::move(sd.vertices), std::move(sd.indices));
    MeshTopology topo = MeshTopology::Build(*finalMesh);
    RecomputeNormalsWelded(*finalMesh, topo);
    finalMesh->RecomputeBounds();
    finalMesh->UploadVertices();
    e->mesh = finalMesh;
    Readopt(m_EditMesh, m_Topology, m_MeshAtEnter, m_MeshVersionSeen, *e->mesh);

    // Leave the inserted midpoints selected as vertices so they're ready to move.
    m_Mode = Element::Vertex;
    m_Selected = RemapNewVerts(m_EditMesh, sd.newVerts);
    return std::make_unique<MeshSwapCommand>(m_Target, original, finalMesh);
}

// --- smooth + shading (T6, #65) ----------------------------------------------

namespace {
// Sparse vertex diff -> one undo step (same shape as EndTransform): only the
// vertices that actually changed go on the stack. nullptr if nothing changed.
std::unique_ptr<Command> DiffStroke(UUID target, const std::vector<Vertex>& before,
                                    const std::vector<Vertex>& after)
{
    std::vector<uint32_t> indices;
    std::vector<Vertex> from, to;
    for (uint32_t i = 0; i < (uint32_t)after.size() && i < (uint32_t)before.size(); ++i) {
        if (std::memcmp(&after[i], &before[i], sizeof(Vertex)) != 0) {
            indices.push_back(i);
            from.push_back(before[i]);
            to.push_back(after[i]);
        }
    }
    if (indices.empty())
        return nullptr;
    return std::make_unique<SculptStrokeCommand>(target, std::move(indices), std::move(from), std::move(to));
}
} // namespace

std::unique_ptr<Command> EditTool::SmoothSelection(Scene& scene, float strength, int iterations)
{
    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh || !CanSmooth())
        return nullptr;
    std::vector<uint32_t> sel = ResolveVertexSet(m_EditMesh, m_Mode, m_Selected);
    if (sel.empty())
        return nullptr;

    std::vector<vec3> pos(m_EditMesh.vertices.size());
    for (size_t i = 0; i < pos.size(); ++i)
        pos[i] = m_EditMesh.vertices[i].position;
    std::vector<vec3> out = LaplacianSmooth(pos, BuildVertexAdjacency(m_EditMesh), sel, strength, iterations);

    std::vector<Vertex> before = e->mesh->Vertices(); // snapshot for the undo diff
    auto& verts = e->mesh->MutableVertices();
    for (uint32_t g : sel) {
        m_EditMesh.vertices[g].position = out[g]; // keep the overlay tracking
        for (uint32_t raw : m_EditMesh.vertices[g].rawVerts)
            if (raw < verts.size())
                verts[raw].position = out[g];
    }
    RecomputeNormalsWelded(*e->mesh, m_Topology);
    e->mesh->RecomputeBounds();
    e->mesh->UploadVertices();
    m_MeshVersionSeen = e->mesh->Version(); // our own in-place edit — no resync
    return DiffStroke(m_Target, before, e->mesh->Vertices());
}

std::unique_ptr<Command> EditTool::SetShading(Scene& scene, bool smooth)
{
    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh || !m_Active)
        return nullptr;
    std::vector<Vertex> before = e->mesh->Vertices();
    if (smooth)
        RecomputeNormalsWelded(*e->mesh, m_Topology);
    else
        RecomputeNormalsFlat(*e->mesh);
    e->mesh->UploadVertices();
    m_MeshVersionSeen = e->mesh->Version(); // normals only, positions unchanged
    return DiffStroke(m_Target, before, e->mesh->Vertices());
}

} // namespace forge
