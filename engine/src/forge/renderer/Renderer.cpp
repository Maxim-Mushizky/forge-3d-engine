#include "Renderer.h"

#include "forge/assets/MeshFactory.h"
#include "forge/core/Log.h"

#include <GL/glew.h>

#include <algorithm>

namespace forge {

static std::string AssetPath(const char* relative)
{
    return std::string(FORGE_ASSET_DIR) + "/" + relative;
}

void Renderer::Init()
{
    glEnable(GL_DEPTH_TEST);

    m_BlinnPhong = std::make_unique<Shader>(AssetPath("shaders/blinnphong.vert"), AssetPath("shaders/blinnphong.frag"));
    m_Flat = std::make_unique<Shader>(AssetPath("shaders/flat.vert"), AssetPath("shaders/flat.frag"));
    m_PBR = std::make_unique<Shader>(AssetPath("shaders/pbr.vert"), AssetPath("shaders/pbr.frag"));
    m_GridShader = std::make_unique<Shader>(AssetPath("shaders/grid.vert"), AssetPath("shaders/grid.frag"));
    m_ShadowDepth = std::make_unique<Shader>(AssetPath("shaders/shadow.vert"), AssetPath("shaders/shadow.frag"));
    m_Sky = std::make_unique<Shader>(AssetPath("shaders/skydome.vert"), AssetPath("shaders/skydome.frag"));
    m_GridPlane = MeshFactory::Plane(400.0f);
    m_SkyDome = MeshFactory::Sphere(16, 24);

    // Shadow map: depth-only framebuffer. Border = max depth so geometry
    // outside the light frustum samples as "lit".
    glGenTextures(1, &m_ShadowTex);
    glBindTexture(GL_TEXTURE_2D, m_ShadowTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, kShadowResolution, kShadowResolution, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);

    glGenFramebuffers(1, &m_ShadowFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ShadowFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_ShadowTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    FORGE_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "shadow framebuffer incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Renderer::BeginScene(const mat4& viewProjection, const vec3& cameraPosition, const DirectionalLight& light)
{
    m_ViewProjection = viewProjection;
    m_CameraPosition = cameraPosition;
    m_Light = light;
    m_Queue.clear();
    m_PointLights.clear();
    m_Outlines.clear();
}

void Renderer::Submit(const Mesh& mesh, const mat4& transform, const Material& material, bool castShadow)
{
    m_Queue.push_back({&mesh, transform, material, castShadow});
}

void Renderer::SubmitLight(const vec3& position, const vec3& color, float intensity, float range)
{
    if ((int)m_PointLights.size() < kMaxPointLights)
        m_PointLights.push_back({position, color * intensity, range});
}

void Renderer::AddOutline(const Mesh& mesh, const mat4& transform, const vec3& color)
{
    m_Outlines.push_back({&mesh, transform, color});
}

AABB Renderer::SceneBounds() const
{
    AABB bounds;
    for (const DrawItem& item : m_Queue) {
        const AABB& lb = item.mesh->Bounds();
        for (int i = 0; i < 8; ++i) {
            vec3 corner{i & 1 ? lb.max.x : lb.min.x,
                        i & 2 ? lb.max.y : lb.min.y,
                        i & 4 ? lb.max.z : lb.min.z};
            bounds.Expand(vec3(item.transform * vec4(corner, 1.0f)));
        }
    }
    return bounds;
}

void Renderer::ShadowPass()
{
    AABB bounds = SceneBounds();
    vec3 center = (bounds.min + bounds.max) * 0.5f;
    float radius = glm::length(bounds.max - bounds.min) * 0.5f + 1.0f;

    vec3 L = glm::normalize(m_Light.direction);
    vec3 up = std::abs(L.y) > 0.99f ? vec3(0, 0, 1) : vec3(0, 1, 0);
    mat4 view = glm::lookAt(center - L * radius * 2.0f, center, up);
    mat4 proj = glm::ortho(-radius, radius, -radius, radius, 0.1f, radius * 4.0f);
    m_LightSpace = proj * view;

    glBindFramebuffer(GL_FRAMEBUFFER, m_ShadowFBO);
    glViewport(0, 0, kShadowResolution, kShadowResolution);
    glClear(GL_DEPTH_BUFFER_BIT);

    m_ShadowDepth->Bind();
    m_ShadowDepth->SetMat4("u_LightSpace", m_LightSpace);
    for (const DrawItem& item : m_Queue) {
        if (!item.castShadow)
            continue;
        m_ShadowDepth->SetMat4("u_Model", item.transform);
        item.mesh->Draw();
    }
}

void Renderer::SkyPass()
{
    // Standard skybox: drawn after opaque geometry at depth 1.0 (z=w in the
    // vertex shader), so it fills exactly the background pixels.
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE); // viewed from inside
    m_Sky->Bind();
    m_Sky->SetMat4("u_ViewProj", m_ViewProjection);
    m_Sky->SetVec3("u_CamPos", m_CameraPosition);
    m_Sky->SetInt("u_Env", 0);
    m_Sky->SetFloat("u_Intensity", m_Environment->intensity);
    m_Sky->SetFloat("u_Rotation", glm::radians(m_Environment->rotationDegrees));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_Environment->Source());
    m_SkyDome->Draw();
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
}

void Renderer::DrawItemMain(const DrawItem& item)
{
    const Material& material = item.material;
    bool hasAlbedoMap = material.albedoMap != nullptr;
    bool hasMRMap = material.metallicRoughnessMap != nullptr;
    bool hasEnv = m_Environment && m_Environment->Valid();
    if (hasAlbedoMap)
        material.albedoMap->Bind(0);
    if (hasMRMap)
        material.metallicRoughnessMap->Bind(1);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, m_ShadowTex);
    if (hasEnv) {
        glActiveTexture(GL_TEXTURE6);
        glBindTexture(GL_TEXTURE_2D, m_Environment->Irradiance());
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_2D, m_Environment->Prefiltered());
    }

    Shader* shader = m_Mode == ShadingMode::Flat ? m_Flat.get()
                   : m_Mode == ShadingMode::BlinnPhong ? m_BlinnPhong.get()
                                                       : m_PBR.get();
    shader->Bind();
    shader->SetMat4("u_ViewProj", m_ViewProjection);
    shader->SetMat4("u_Model", item.transform);
    shader->SetVec3("u_Albedo", material.albedo);
    shader->SetInt("u_AlbedoMap", 0);
    shader->SetInt("u_HasAlbedoMap", hasAlbedoMap ? 1 : 0);

    if (m_Mode != ShadingMode::Flat) {
        shader->SetMat3("u_NormalMatrix", mat3(glm::transpose(glm::inverse(item.transform))));
        shader->SetVec3("u_CamPos", m_CameraPosition);
        shader->SetVec3("u_LightDir", m_Light.direction);
        shader->SetVec3("u_LightColor", m_Light.color);
        shader->SetFloat("u_LightIntensity", m_Light.intensity);
        shader->SetMat4("u_LightSpace", m_LightSpace);
        shader->SetInt("u_ShadowMap", 5);
        shader->SetInt("u_ShadowsEnabled", m_ShadowsThisFrame ? 1 : 0);
        shader->SetVec3("u_Emissive", material.emissive * material.emissiveStrength);

        shader->SetInt("u_NumPointLights", (int)m_PointLights.size());
        for (int i = 0; i < (int)m_PointLights.size(); ++i) {
            std::string base = "u_PointLights[" + std::to_string(i) + "].";
            shader->SetVec3(base + "position", m_PointLights[i].position);
            shader->SetVec3(base + "color", m_PointLights[i].color);
            shader->SetFloat(base + "range", m_PointLights[i].range);
        }
    }
    if (m_Mode == ShadingMode::PBR) {
        shader->SetFloat("u_Metallic", material.metallic);
        shader->SetFloat("u_Roughness", material.roughness);
        shader->SetFloat("u_Transmission", material.transmission);
        shader->SetInt("u_MRMap", 1);
        shader->SetInt("u_HasMRMap", hasMRMap ? 1 : 0);
        shader->SetInt("u_EnvIrradiance", 6);
        shader->SetInt("u_EnvPrefiltered", 7);
        shader->SetInt("u_HasEnv", hasEnv ? 1 : 0);
        shader->SetFloat("u_EnvIntensity", hasEnv ? m_Environment->intensity : 0.0f);
        shader->SetFloat("u_EnvRotation", hasEnv ? glm::radians(m_Environment->rotationDegrees) : 0.0f);
    }
    item.mesh->Draw();
}

void Renderer::EndScene(const Framebuffer& target)
{
    m_ShadowsThisFrame = m_ShadowsEnabled && !m_Queue.empty() && m_Mode != ShadingMode::Flat;
    if (m_ShadowsThisFrame)
        ShadowPass();

    target.Bind();
    // Linear-space background (the post stack gamma-encodes it back to the old look).
    glClearColor(0.010f, 0.012f, 0.016f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    for (const DrawItem& item : m_Queue)
        if (item.material.transmission <= 0.0f)
            DrawItemMain(item);

    if (m_Environment && m_Environment->Valid())
        SkyPass(); // after opaques: fills only background pixels (depth = 1)

    // Transmissive pass: after the sky so the background shows through, blended
    // back-to-front, depth test on but writes off (glass shouldn't occlude).
    std::vector<const DrawItem*> transmissive;
    for (const DrawItem& item : m_Queue)
        if (item.material.transmission > 0.0f)
            transmissive.push_back(&item);
    if (!transmissive.empty()) {
        std::sort(transmissive.begin(), transmissive.end(), [&](const DrawItem* a, const DrawItem* b) {
            vec3 va = vec3(a->transform[3]) - m_CameraPosition;
            vec3 vb = vec3(b->transform[3]) - m_CameraPosition;
            float da = glm::dot(va, va), db = glm::dot(vb, vb);
            return da > db; // far first
        });
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        for (const DrawItem* item : transmissive)
            DrawItemMain(*item);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }

    if (!m_Outlines.empty()) {
        m_Flat->Bind();
        m_Flat->SetMat4("u_ViewProj", m_ViewProjection);
        m_Flat->SetInt("u_HasAlbedoMap", 0);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(-1.0f, -1.0f); // pull lines toward the camera so they win the depth test
        for (const OutlineItem& o : m_Outlines) {
            m_Flat->SetMat4("u_Model", o.transform);
            m_Flat->SetVec3("u_Color", o.color);
            o.mesh->Draw();
        }
        glDisable(GL_POLYGON_OFFSET_LINE);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }

    // Grid: blended, two-sided, drawn last so it composites over the background.
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    m_GridShader->Bind();
    m_GridShader->SetMat4("u_ViewProj", m_ViewProjection);
    m_GridShader->SetVec3("u_CamPos", m_CameraPosition);
    m_GridShader->SetMat4("u_LightSpace", m_LightSpace);
    m_GridShader->SetInt("u_ShadowMap", 5);
    m_GridShader->SetInt("u_ShadowsEnabled", m_ShadowsThisFrame ? 1 : 0);
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, m_ShadowTex);
    m_GridPlane->Draw();
    glDisable(GL_BLEND);
}

} // namespace forge
