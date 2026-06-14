#pragma once

#include "CommandStack.h"

#include <forge/geometry/EditMesh.h>
#include <forge/geometry/MeshEdit.h>
#include <forge/scene/Scene.h>

#include <cstdint>
#include <memory>
#include <vector>

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
    using Element = ElementKind; // shared with the engine helpers

    bool Active() const { return m_Active; }
    UUID Target() const { return m_Target; }
    Element Mode() const { return m_Mode; }
    void SetMode(Element mode); // switching element type clears the selection
    bool HasSelection() const { return !m_Selected.empty(); }

    void Enter(Scene& scene, UUID entity);
    void Exit();

    // Per-frame from the viewport: validates the target, then draws the element
    // overlay into the current window's draw list. Self-exits (and draws
    // nothing) if the target vanished or its mesh was swapped.
    void DrawOverlay(Scene& scene, const EditorCamera& camera, const vec2& viewportPos,
                     const vec2& viewportSize);

    // Click-pick the active element type nearest cursorPx. additive = Ctrl held
    // (toggle); otherwise replaces the selection (a miss clears it).
    void Pick(Scene& scene, const EditorCamera& camera, const vec2& viewportPos,
              const vec2& viewportSize, const vec2& cursorPx, bool additive);
    // Box-select every active element whose representative point falls in the
    // pixel rect [rectMin, rectMax]. additive keeps the current selection.
    void BoxPick(Scene& scene, const EditorCamera& camera, const vec2& viewportPos,
                 const vec2& viewportSize, const vec2& rectMin, const vec2& rectMax, bool additive);

    size_t SelectionCount() const { return m_Selected.size(); }

    // --- element transform (T2) ----------------------------------------------
    // Object-space centroid of the current selection (origin if empty).
    vec3 SelectionCentroidObject() const;
    // Begin a gizmo drag: snapshot the affected vertices + the whole mesh for
    // the undo diff. ApplyTransform maps those start positions by an
    // object-space delta each frame; EndTransform commits one undo step.
    void BeginTransform(Scene& scene);
    void ApplyTransform(Scene& scene, const mat4& objectXform);
    std::unique_ptr<Command> EndTransform(Scene& scene);

private:
    bool m_Active = false;
    UUID m_Target = 0;
    Mesh* m_MeshAtEnter = nullptr;     // staleness guard: mesh swapped under us -> exit
    uint64_t m_MeshVersionSeen = 0;    // detect in-place edits (undo/redo) to refresh the snapshot
    Element m_Mode = Element::Vertex;
    EditMesh m_EditMesh;
    MeshTopology m_Topology;          // for welded-normal recompute after edits
    std::vector<uint32_t> m_Selected; // ids into the active element vector (cleared on mode switch)

    // drag state
    std::vector<uint32_t> m_DragVerts;    // affected EditVertex ids
    std::vector<vec3> m_DragStartPos;     // their object-space positions at drag start (parallel)
    std::vector<Vertex> m_MeshBefore;     // full vertex snapshot for the sparse undo diff
};

} // namespace forge
