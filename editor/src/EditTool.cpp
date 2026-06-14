#include "EditTool.h"

#include "EditorCamera.h"

#include <forge/core/Log.h>

#include <imgui.h>

namespace forge {

namespace {
// Crease threshold for which edges read as real wireframe lines; flat-face
// triangulation diagonals fall below it and stay hidden (see EditMesh).
constexpr float kCreaseThreshold = 0.5236f; // 30 degrees in radians

constexpr ImU32 kEdgeBase = IM_COL32(210, 214, 224, 150);
constexpr ImU32 kAccent = IM_COL32(240, 148, 56, 255); // matches the edit-mode border
constexpr ImU32 kVertBase = IM_COL32(150, 160, 175, 180);
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
            dl->AddLine(a, b, edgeMode ? kAccent : kEdgeBase, edgeMode ? 2.0f : 1.3f);
    }

    // Vertices: always shown so the cage is legible, emphasised in Vertex mode.
    for (const EditVertex& vert : m_EditMesh.vertices) {
        ImVec2 s;
        if (project(vert.position, s))
            dl->AddCircleFilled(s, vertMode ? 4.0f : 2.0f, vertMode ? kAccent : kVertBase);
    }

    // Face centroids: only marked in Face mode (where faces are the target).
    if (faceMode) {
        for (const EditFace& face : m_EditMesh.faces) {
            ImVec2 s;
            if (project(face.centroid, s))
                dl->AddCircleFilled(s, 3.5f, kAccent);
        }
    }
}

} // namespace forge
