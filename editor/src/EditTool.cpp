#include "EditTool.h"

#include "EditorCamera.h"

#include <forge/core/Log.h>

#include <imgui.h>

#include <algorithm>

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
    // Read-only in T1b/T1c — no copy-on-write clone yet (that arrives with
    // element transforms in T2). Just snapshot the topology for display.
    m_EditMesh = BuildEditMesh(*e->mesh);
    m_Target = entity;
    m_MeshAtEnter = e->mesh.get();
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
    m_Selected.clear();
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

} // namespace forge
