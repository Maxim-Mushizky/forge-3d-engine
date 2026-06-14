#pragma once

#include <forge/geometry/EditMesh.h>
#include <forge/scene/Scene.h>

namespace forge {

class EditorCamera;

// Edit Mode (#53): a vertex/edge/face overlay over the selected mesh — the
// shell that selection (T1c) and element transforms (T2) build on. Mirrors
// SculptTool's mode lifecycle (Enter/Exit + staleness guards) but mutates
// nothing yet, so it does NOT clone the mesh: it snapshots an EditMesh for
// display and self-exits if the underlying mesh is swapped out (a topology-op
// undo/redo) or the entity is deleted — the same guard SculptTool uses.
class EditTool {
public:
    enum class Element { Vertex, Edge, Face };

    bool Active() const { return m_Active; }
    UUID Target() const { return m_Target; }
    Element Mode() const { return m_Mode; }
    void SetMode(Element mode) { m_Mode = mode; }

    void Enter(Scene& scene, UUID entity);
    void Exit();

    // Per-frame from the viewport: validates the target, then draws the element
    // overlay into the current window's draw list. Self-exits (and draws
    // nothing) if the target vanished or its mesh was swapped.
    void DrawOverlay(Scene& scene, const EditorCamera& camera, const vec2& viewportPos,
                     const vec2& viewportSize);

private:
    bool m_Active = false;
    UUID m_Target = 0;
    Mesh* m_MeshAtEnter = nullptr; // staleness guard: mesh swapped under us -> exit
    Element m_Mode = Element::Vertex;
    EditMesh m_EditMesh;
};

} // namespace forge
