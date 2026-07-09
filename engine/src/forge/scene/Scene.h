#pragma once

#include "forge/anim/Pose.h"
#include "forge/anim/Skeleton.h"
#include "forge/core/Geometry.h"
#include "forge/core/UUID.h"
#include "forge/renderer/Mesh.h"
#include "forge/scene/Components.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace forge {

struct LightComponent {
    bool enabled = false;
    vec3 color{1.0f, 0.95f, 0.85f};
    float intensity = 20.0f; // radiance at 1m
    float range = 15.0f;
};

// Every editor object currently has the same component set, so Entity is a plain
// struct (copyable — the undo system snapshots whole entities). If component
// variety grows (scripts, physics), this migrates to the registry design in PLAN.md.
struct Entity {
    UUID id = 0;
    UUID parent = 0; // 0 = root; transform is parent-relative (local)
    std::string name;
    TransformComponent transform;
    std::shared_ptr<Mesh> mesh; // null = empty node (e.g. a group)
    std::shared_ptr<Skeleton> skeleton; // null = not skinned
    Pose pose; // per-joint FK overrides on top of the skeleton bind; empty = bind (#147)
    std::vector<float> morphWeights; // per-instance morph target weights; empty = all zero (#149)
    MaterialComponent material; // material slot 0 — kept as a direct field so
                                // every single-material call site stays valid (#80)
    std::vector<MaterialComponent> extraMaterials; // slots 1+ (multi-material meshes)
    LightComponent light; // point light when enabled (the mesh is just its gizmo)
};

// Material slot access (#80): slot 0 = Entity::material, slot n>=1 =
// extraMaterials[n-1]. Out-of-range slots fall back to slot 0 so a mesh whose
// submeshes reference more slots than the entity carries still renders.
inline uint32_t MaterialSlotCount(const Entity& e) { return 1 + (uint32_t)e.extraMaterials.size(); }

inline const MaterialComponent& MaterialForSlot(const Entity& e, uint32_t slot)
{
    return (slot == 0 || slot > e.extraMaterials.size()) ? e.material : e.extraMaterials[slot - 1];
}

inline MaterialComponent& MaterialForSlot(Entity& e, uint32_t slot)
{
    return (slot == 0 || slot > e.extraMaterials.size()) ? e.material : e.extraMaterials[slot - 1];
}

struct RaycastHit {
    UUID entity = 0;
    float distance = 0.0f;
    vec3 worldPos{0.0f};
    vec3 worldNormal{0.0f, 1.0f, 0.0f};
    uint32_t triIndex = 0; // first index of the hit triangle (into mesh indices)
};

class Scene {
public:
    Entity& CreateEntity(const std::string& name);
    void Insert(const Entity& entity);  // restore a snapshot (undo/redo), keeps its id
    void Replace(const Entity& entity); // overwrite by id
    bool Remove(UUID id);
    Entity* Find(UUID id);
    const Entity* Find(UUID id) const;

    std::vector<Entity>& Entities() { return m_Entities; }
    const std::vector<Entity>& Entities() const { return m_Entities; }

    // --- hierarchy ----------------------------------------------------------
    mat4 WorldTransform(UUID id) const; // identity for id==0/unknown
    std::vector<UUID> ChildrenOf(UUID id) const;
    bool IsDescendantOf(UUID maybeChild, UUID maybeAncestor) const;
    UUID RootAncestor(UUID id) const; // topmost ancestor (the entity itself if unparented)
    // Change parent while keeping the entity's world pose (best-effort TRS decompose).
    void ReparentKeepWorld(UUID id, UUID newParent);

    // Closest hit along a world-space ray. AABB reject, then exact triangle test.
    // onlyEntity != 0 restricts the test to that entity (sculpt brushes).
    std::optional<RaycastHit> Raycast(const Ray& worldRay, UUID onlyEntity = 0) const;

private:
    std::vector<Entity> m_Entities;
};

} // namespace forge
