#include "EditTool.h"

#include "EditorCamera.h"

#include <forge/core/Log.h>

#include <imgui.h>

namespace forge {

namespace {
// Crease threshold for which edges read as real wireframe lines; flat-face
// triangulation diagonals fall below it and stay hidden (see EditMesh).
constexpr float kCreaseThreshold = 0.5236f; // 30 degrees in radians

// High-contrast palette: meshes are often warm/orange, so verts use a
// white fill with a dark ring (legible on any albedo or against the grid),
// edges use cyan, and the emphasised element switches to a saturated colour.
constexpr ImU32 kEdge = IM_COL32(50, 190, 255, 220);    // cyan wireframe
constexpr ImU32 kEdgeHot = IM_COL32(130, 220, 255, 255); // emphasised in Edge mode
constexpr ImU32 kVertFill = IM_COL32(245, 246, 250, 255);
constexpr ImU32 kVertHot = IM_COL32(90, 150, 255, 255);  // emphasised in Vertex mode
constexpr ImU32 kOutline = IM_COL32(15, 16, 22, 255);    // dark ring for contrast
constexpr ImU32 kFaceHot = IM_COL32(255, 210, 40, 255);  // amber face dots
} // namespace

void EditTool::Enter(Scene& scene, UUID entity)
{
    Entity* e = scene.Find(entity);
    if (!e || !e->mesh)
        return;
    // Read-only in T1b — no copy-on-write clone yet (that arrives with element
    // transforms in T2). Just snapshot the topology for display.
    m_EditMesh = BuildEditMesh(*e->mesh);
    m_Target = entity;
    m_MeshAtEnter = e->mesh.get();
    m_Active = true;
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

    // Object-space point -> viewport pixel. Returns false for points behind the
    // camera (w <= 0), which must be culled before the perspective divide.
    auto project = [&](const vec3& p, ImVec2& out) -> bool {
        vec4 c = mvp * vec4(p, 1.0f);
        if (c.w <= 0.0f)
            return false;
        c /= c.w;
        out = ImVec2(viewportPos.x + (c.x * 0.5f + 0.5f) * viewportSize.x,
                     viewportPos.y + (1.0f - (c.y * 0.5f + 0.5f)) * viewportSize.y);
        return true;
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool vertMode = m_Mode == Element::Vertex;
    const bool edgeMode = m_Mode == Element::Edge;
    const bool faceMode = m_Mode == Element::Face;

    // Wireframe: crease edges only, so flat-face diagonals stay hidden. Edges
    // are emphasised (brighter + thicker) while in Edge mode.
    for (const EditEdge& edge : m_EditMesh.edges) {
        if (!IsCreaseEdge(edge, kCreaseThreshold))
            continue;
        ImVec2 a, b;
        if (project(m_EditMesh.vertices[edge.v0].position, a) &&
            project(m_EditMesh.vertices[edge.v1].position, b))
            dl->AddLine(a, b, edgeMode ? kEdgeHot : kEdge, edgeMode ? 2.4f : 1.5f);
    }

    // Vertices: always shown so the cage is legible, emphasised in Vertex mode.
    // White fill + dark ring stays visible on any mesh colour or the grid.
    for (const EditVertex& vert : m_EditMesh.vertices) {
        ImVec2 s;
        if (!project(vert.position, s))
            continue;
        float r = vertMode ? 5.0f : 3.0f;
        dl->AddCircleFilled(s, r, vertMode ? kVertHot : kVertFill);
        dl->AddCircle(s, r, kOutline, 0, 1.5f);
    }

    // Face centroids: only marked in Face mode (where faces are the target).
    if (faceMode) {
        for (const EditFace& face : m_EditMesh.faces) {
            ImVec2 s;
            if (!project(face.centroid, s))
                continue;
            dl->AddCircleFilled(s, 4.0f, kFaceHot);
            dl->AddCircle(s, 4.0f, kOutline, 0, 1.5f);
        }
    }
}

} // namespace forge
