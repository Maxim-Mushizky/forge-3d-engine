#include "SculptTool.h"
#include "EditorCamera.h"
#include "Theme.h"

#include <forge/core/Log.h>

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace forge {

static float Falloff(float t) // smoothstep-shaped, 1 at center -> 0 at radius
{
    t = glm::clamp(t, 0.0f, 1.0f);
    return 1.0f - (3.0f * t * t - 2.0f * t * t * t);
}

void SculptTool::Enter(Scene& scene, UUID entity)
{
    Entity* e = scene.Find(entity);
    if (!e || !e->mesh)
        return;

    // Copy-on-write: primitives are shared between entities AND undo snapshots
    // hold the same shared_ptr — sculpting in place would edit siblings/history.
    if (e->mesh.use_count() > 1)
        e->mesh = std::make_shared<Mesh>(e->mesh->Vertices(), e->mesh->Indices());

    m_Topology = MeshTopology::Build(*e->mesh);
    m_Target = entity;
    m_Active = true;
    m_Stroking = false;
    FORGE_INFO("Sculpt: %s (%zu verts, %zu weld groups)", e->name.c_str(),
               e->mesh->Vertices().size(), m_Topology.groups.size());
}

void SculptTool::Exit()
{
    m_Active = false;
    m_Stroking = false;
    m_Target = 0;
    m_Topology = {};
}

void SculptTool::DrawSettingsUI()
{
    // 2x2 brush grid, accent-highlighted selection.
    const float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    auto brushButton = [&](const char* label, Brush b, const char* tip, bool sameLine) {
        bool active = m_Brush == b;
        if (active)
            ui::PushAccentButton();
        if (ImGui::Button(label, ImVec2(halfW, 28)))
            m_Brush = b;
        if (active)
            ui::PopAccentButton();
        ImGui::SetItemTooltip("%s", tip);
        if (sameLine)
            ImGui::SameLine();
    };
    brushButton("Draw", Brush::Draw, "Pull the surface out (Ctrl pushes in)", true);
    brushButton("Smooth", Brush::Smooth, "Relax bumps and wrinkles", false);
    brushButton("Grab", Brush::Grab, "Drag a region around", true);
    brushButton("Inflate", Brush::Inflate, "Puff the surface outward", false);

    ImGui::SliderFloat("Radius", &m_Radius, 0.05f, 2.0f);
    ImGui::SetItemTooltip("Size of the brush circle");
    ImGui::SliderFloat("Strength", &m_Strength, 0.05f, 1.0f);
    ImGui::SetItemTooltip("How much each stroke moves the surface");
    ImGui::Checkbox("X mirror", &m_MirrorX);
    ImGui::SetItemTooltip("Edits apply to both left and right sides");
}

void SculptTool::CaptureGrab(Entity& e, const vec3& localPos, float localRadius, bool mirrored)
{
    const auto& verts = e.mesh->Vertices();
    for (const auto& group : m_Topology.groups) {
        const vec3& rep = verts[group[0]].position;
        float w = Falloff(glm::length(rep - localPos) / localRadius);
        if (w <= 0.001f)
            continue;
        for (uint32_t i : group)
            m_GrabVerts.push_back({i, w, verts[i].position, mirrored});
    }
}

void SculptTool::BeginStroke(Scene& scene, const RaycastHit& hit, const EditorCamera& camera, bool smoothOverride)
{
    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh)
        return;

    m_Stroking = true;
    m_StrokeIsSmooth = smoothOverride;
    m_StrokeBefore = e->mesh->Vertices(); // transient full snapshot, diffed at stroke end
    m_HaveLastDab = false;

    if (m_Brush == Brush::Grab && !smoothOverride) {
        m_GrabVerts.clear();
        mat4 inv = glm::inverse(scene.WorldTransform(m_Target));
        vec3 localPos = vec3(inv * vec4(hit.worldPos, 1.0f));
        float avgScale = glm::length(vec3(inv[0])) ; // ~1/scale for uniform scale
        float localRadius = m_Radius * avgScale;
        CaptureGrab(*e, localPos, localRadius, false);
        if (m_MirrorX)
            CaptureGrab(*e, vec3(-localPos.x, localPos.y, localPos.z), localRadius, true);
        m_GrabPlanePoint = hit.worldPos;
        m_GrabPlaneNormal = glm::normalize(camera.Position() - camera.FocalPoint());
    }
}

std::unique_ptr<Command> SculptTool::EndStroke(Scene& scene)
{
    m_Stroking = false;
    m_GrabVerts.clear();

    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh)
        return nullptr;

    e->mesh->RecomputeBounds();

    // Sparse diff: only changed vertices go on the undo stack.
    const auto& after = e->mesh->Vertices();
    std::vector<uint32_t> indices;
    std::vector<Vertex> before, now;
    for (uint32_t i = 0; i < (uint32_t)after.size() && i < (uint32_t)m_StrokeBefore.size(); ++i) {
        if (std::memcmp(&after[i], &m_StrokeBefore[i], sizeof(Vertex)) != 0) {
            indices.push_back(i);
            before.push_back(m_StrokeBefore[i]);
            now.push_back(after[i]);
        }
    }
    m_StrokeBefore.clear();
    if (indices.empty())
        return nullptr;
    return std::make_unique<SculptStrokeCommand>(m_Target, std::move(indices), std::move(before), std::move(now));
}

void SculptTool::ApplyDab(Entity& e, const vec3& localPos, const vec3& localDir, float localRadius,
                          float amount, bool smoothMode)
{
    auto& verts = e.mesh->MutableVertices();

    for (size_t g = 0; g < m_Topology.groups.size(); ++g) {
        const auto& group = m_Topology.groups[g];
        const vec3& rep = verts[group[0]].position;
        float w = Falloff(glm::length(rep - localPos) / localRadius);
        if (w <= 0.001f)
            continue;

        vec3 newPos;
        if (smoothMode || m_Brush == Brush::Smooth) {
            const auto& neighbors = m_Topology.groupNeighbors[g];
            if (neighbors.empty())
                continue;
            vec3 avg(0.0f);
            for (uint32_t ng : neighbors)
                avg += verts[m_Topology.groups[ng][0]].position;
            avg /= (float)neighbors.size();
            newPos = glm::mix(rep, avg, glm::clamp(w * m_Strength * 1.5f, 0.0f, 1.0f));
        } else if (m_Brush == Brush::Inflate) {
            newPos = rep + verts[group[0]].normal * (amount * w);
        } else { // Draw
            newPos = rep + localDir * (amount * w);
        }

        for (uint32_t i : group)
            verts[i].position = newPos;
    }
}

std::unique_ptr<Command> SculptTool::OnViewportFrame(Scene& scene, const EditorCamera& camera,
                                                     const vec2& viewportPos, const vec2& viewportSize,
                                                     bool viewportHovered)
{
    if (!m_Active)
        return nullptr;
    Entity* e = scene.Find(m_Target);
    if (!e || !e->mesh) { // target deleted under us
        Exit();
        return nullptr;
    }

    ImGuiIO& io = ImGui::GetIO();

    // Mouse ray through the viewport pixel.
    vec2 uv{(io.MousePos.x - viewportPos.x) / viewportSize.x,
            (io.MousePos.y - viewportPos.y) / viewportSize.y};
    mat4 invVP = glm::inverse(camera.ViewProjection());
    vec4 pNear = invVP * vec4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, -1.0f, 1.0f);
    vec4 pFar = invVP * vec4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 1.0f, 1.0f);
    Ray ray{vec3(pNear) / pNear.w, glm::normalize(vec3(pFar) / pFar.w - vec3(pNear) / pNear.w)};

    auto hit = scene.Raycast(ray, m_Target);

    // --- brush cursor circle -------------------------------------------------
    if (hit && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        vec3 n = hit->worldNormal;
        vec3 t = glm::normalize(glm::cross(std::abs(n.y) < 0.95f ? vec3(0, 1, 0) : vec3(1, 0, 0), n));
        vec3 b = glm::cross(n, t);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        constexpr int kSegments = 48;
        ImVec2 pts[kSegments];
        mat4 vp = camera.ViewProjection();
        for (int i = 0; i < kSegments; ++i) {
            float a = 2.0f * glm::pi<float>() * (float)i / kSegments;
            vec3 w = hit->worldPos + (t * std::cos(a) + b * std::sin(a)) * m_Radius + n * 0.002f;
            vec4 c = vp * vec4(w, 1.0f);
            c /= c.w;
            pts[i] = ImVec2(viewportPos.x + (c.x * 0.5f + 0.5f) * viewportSize.x,
                            viewportPos.y + (1.0f - (c.y * 0.5f + 0.5f)) * viewportSize.y);
        }
        dl->AddPolyline(pts, kSegments, IM_COL32(255, 180, 60, 220), ImDrawFlags_Closed, 1.5f);
    }

    // --- stroke lifecycle ------------------------------------------------------
    bool cameraDrag = io.KeyAlt; // Alt+LMB belongs to the camera even in sculpt mode
    if (!m_Stroking && viewportHovered && hit && !cameraDrag &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        BeginStroke(scene, *hit, camera, io.KeyShift);

    if (m_Stroking) {
        bool changed = false;
        mat4 world = scene.WorldTransform(m_Target);
        mat4 inv = glm::inverse(world);
        float avgInvScale = glm::length(vec3(inv[0]));
        float localRadius = m_Radius * avgInvScale;

        if (m_Brush == Brush::Grab && !m_StrokeIsSmooth) {
            // Drag captured vertices along the camera-facing plane through the grab point.
            float denom = glm::dot(ray.direction, m_GrabPlaneNormal);
            if (std::abs(denom) > 1e-5f) {
                float t = glm::dot(m_GrabPlanePoint - ray.origin, m_GrabPlaneNormal) / denom;
                vec3 onPlane = ray.origin + ray.direction * t;
                vec3 deltaLocal = mat3(inv) * (onPlane - m_GrabPlanePoint);
                auto& verts = e->mesh->MutableVertices();
                for (const GrabVert& gv : m_GrabVerts) {
                    vec3 d = gv.mirrored ? vec3(-deltaLocal.x, deltaLocal.y, deltaLocal.z) : deltaLocal;
                    verts[gv.index].position = gv.startPos + d * gv.weight;
                }
                changed = true;
            }
        } else if (hit) {
            // Substep dabs along the stroke so fast motion doesn't dot.
            float amount = m_Strength * m_Radius * 0.12f * (io.KeyCtrl ? -1.0f : 1.0f);
            float spacing = m_Radius * 0.35f;
            std::vector<vec3> dabPoints;
            if (!m_HaveLastDab) {
                dabPoints.push_back(hit->worldPos);
            } else {
                vec3 from = m_LastDabWorld, to = hit->worldPos;
                float dist = glm::length(to - from);
                int steps = std::min((int)(dist / spacing), 16);
                for (int s = 1; s <= steps; ++s)
                    dabPoints.push_back(glm::mix(from, to, (float)s / steps));
            }
            for (const vec3& wp : dabPoints) {
                vec3 lp = vec3(inv * vec4(wp, 1.0f));
                vec3 ld = glm::normalize(mat3(inv) * hit->worldNormal);
                ApplyDab(*e, lp, ld, localRadius, amount, m_StrokeIsSmooth);
                if (m_MirrorX)
                    ApplyDab(*e, vec3(-lp.x, lp.y, lp.z), vec3(-ld.x, ld.y, ld.z), localRadius,
                             amount, m_StrokeIsSmooth);
                m_LastDabWorld = wp;
                m_HaveLastDab = true;
                changed = true;
            }
        }

        if (changed) {
            RecomputeNormalsWelded(*e->mesh, m_Topology);
            e->mesh->UploadVertices();
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
            return EndStroke(scene);
    }

    return nullptr;
}

} // namespace forge
