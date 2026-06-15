#pragma once

#include "CommandStack.h"

#include <forge/geometry/EditMesh.h>
#include <forge/geometry/MeshEdit.h>
#include <forge/scene/Scene.h>

#include <cstdint>
#include <memory>
#include <utility>
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

    // --- extrude (T3 faces #63, T4 edges #64) --------------------------------
    // Push-pull the selected faces (cap + walls) or pull the selected edges (new
    // bridging quads) along a computed normal. The editor arms it with
    // BeginExtrude (which swaps in the zero-offset geometry and stays in edit
    // mode), drives UpdateExtrude as the mouse slides, then commits with
    // EndExtrude (one MeshSwapCommand, the new geometry left selected) or aborts
    // with CancelExtrude. Face or Edge mode with a live selection can extrude.
    bool CanExtrude() const
    {
        return m_Active && (m_Mode == Element::Face || m_Mode == Element::Edge) && !m_Selected.empty() &&
               !m_Extruding;
    }
    bool Extruding() const { return m_Extruding; }
    // Object-space anchor (cap centroid at offset 0) and slide direction — the
    // editor maps these to world space to build the drag line.
    vec3 ExtrudeAnchorObject() const { return m_ExtrudeAnchor; }
    vec3 ExtrudeNormalObject() const { return m_ExtrudeNormal; }
    bool BeginExtrude(Scene& scene);
    void UpdateExtrude(Scene& scene, float offset);
    std::unique_ptr<Command> EndExtrude(Scene& scene);
    void CancelExtrude(Scene& scene);

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

    // extrude state
    bool m_Extruding = false;
    std::shared_ptr<Mesh> m_ExtrudeOriginal; // pre-extrude mesh, for undo + cancel
    std::vector<uint32_t> m_ExtrudeMoving;   // raw vert indices that slide (cap + wall tops)
    std::vector<vec3> m_ExtrudeBase;         // their object-space positions at offset 0 (parallel)
    vec3 m_ExtrudeNormal{0.0f, 1.0f, 0.0f};  // object-space slide direction
    vec3 m_ExtrudeAnchor{0.0f};              // new-geometry centroid at offset 0 (drag-line anchor)
    float m_ExtrudeLastOffset = 0.0f;
    // Edge extrude (#64) only: new top-edge endpoint pairs (raw vert indices),
    // remapped to EditEdge ids after commit so the new edges stay selected.
    // Empty for a face extrude, whose cap keeps its stable triangle ids.
    std::vector<std::pair<uint32_t, uint32_t>> m_ExtrudeNewEdges;
};

} // namespace forge
