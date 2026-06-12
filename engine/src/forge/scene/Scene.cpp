#include "Scene.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#include <algorithm>

namespace forge {

Entity& Scene::CreateEntity(const std::string& name)
{
    Entity& e = m_Entities.emplace_back();
    e.id = GenerateUUID();
    e.name = name;
    return e;
}

void Scene::Insert(const Entity& entity)
{
    m_Entities.push_back(entity);
}

void Scene::Replace(const Entity& entity)
{
    if (Entity* e = Find(entity.id))
        *e = entity;
}

bool Scene::Remove(UUID id)
{
    auto it = std::find_if(m_Entities.begin(), m_Entities.end(),
                           [id](const Entity& e) { return e.id == id; });
    if (it == m_Entities.end())
        return false;
    m_Entities.erase(it);
    return true;
}

Entity* Scene::Find(UUID id)
{
    auto it = std::find_if(m_Entities.begin(), m_Entities.end(),
                           [id](const Entity& e) { return e.id == id; });
    return it == m_Entities.end() ? nullptr : &*it;
}

const Entity* Scene::Find(UUID id) const
{
    return const_cast<Scene*>(this)->Find(id);
}

mat4 Scene::WorldTransform(UUID id) const
{
    mat4 world(1.0f);
    int guard = 0; // cycle safety
    for (const Entity* e = Find(id); e && guard < 64; e = Find(e->parent), ++guard)
        world = e->transform.World() * world;
    return world;
}

std::vector<UUID> Scene::ChildrenOf(UUID id) const
{
    std::vector<UUID> children;
    for (const Entity& e : m_Entities)
        if (e.parent == id && id != 0)
            children.push_back(e.id);
    return children;
}

bool Scene::IsDescendantOf(UUID maybeChild, UUID maybeAncestor) const
{
    int guard = 0;
    for (const Entity* e = Find(maybeChild); e && guard < 64; e = Find(e->parent), ++guard)
        if (e->parent == maybeAncestor)
            return true;
    return false;
}

UUID Scene::RootAncestor(UUID id) const
{
    UUID current = id;
    int guard = 0;
    for (const Entity* e = Find(current); e && e->parent != 0 && guard < 64; ++guard) {
        const Entity* p = Find(e->parent);
        if (!p)
            break;
        current = p->id;
        e = p;
    }
    return current;
}

void Scene::ReparentKeepWorld(UUID id, UUID newParent)
{
    Entity* e = Find(id);
    if (!e || newParent == id || IsDescendantOf(newParent, id))
        return;

    mat4 world = WorldTransform(id);
    mat4 newLocal = glm::inverse(WorldTransform(newParent)) * world;

    vec3 scale, translation, skew;
    vec4 perspective;
    quat orientation;
    if (glm::decompose(newLocal, scale, orientation, translation, skew, perspective)) {
        e->transform.translation = translation;
        e->transform.rotation = glm::eulerAngles(orientation);
        e->transform.scale = scale;
    }
    e->parent = newParent;
}

std::optional<RaycastHit> Scene::Raycast(const Ray& worldRay, UUID onlyEntity) const
{
    float bestT = FLT_MAX;
    RaycastHit best;

    for (const Entity& e : m_Entities) {
        if (!e.mesh)
            continue;
        if (onlyEntity != 0 && e.id != onlyEntity)
            continue;

        mat4 world = WorldTransform(e.id);
        mat4 inv = glm::inverse(world);
        Ray local{vec3(inv * vec4(worldRay.origin, 1.0f)),
                  glm::normalize(vec3(inv * vec4(worldRay.direction, 0.0f)))};

        float tBox;
        if (!RayIntersectsAABB(local, e.mesh->Bounds(), tBox))
            continue;

        const auto& verts = e.mesh->Vertices();
        const auto& idx = e.mesh->Indices();
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            float t;
            const vec3& v0 = verts[idx[i]].position;
            const vec3& v1 = verts[idx[i + 1]].position;
            const vec3& v2 = verts[idx[i + 2]].position;
            if (!RayIntersectsTriangle(local, v0, v1, v2, t))
                continue;
            // Local-space t is distorted by non-uniform scale: measure in world space.
            vec3 hitWorld = vec3(world * vec4(local.origin + local.direction * t, 1.0f));
            float tWorld = glm::dot(hitWorld - worldRay.origin, worldRay.direction);
            if (tWorld > 0.0f && tWorld < bestT) {
                bestT = tWorld;
                best.entity = e.id;
                best.distance = tWorld;
                best.worldPos = hitWorld;
                vec3 nLocal = glm::cross(v1 - v0, v2 - v0);
                vec3 nWorld = glm::normalize(mat3(glm::transpose(inv)) * nLocal);
                // face toward the ray origin
                best.worldNormal = glm::dot(nWorld, worldRay.direction) < 0.0f ? nWorld : -nWorld;
                best.triIndex = (uint32_t)i;
            }
        }
    }

    if (best.entity == 0)
        return std::nullopt;
    return best;
}

} // namespace forge
