#include "PathTracer.h"

#include "forge/core/Log.h"
#include "forge/raytrace/BVH.h"

#include <GL/glew.h>

#include <algorithm>
#include <cmath>

namespace forge {

// GPU-side layouts (std430, 16-byte aligned).
struct GPUTriangle {
    vec4 v0; // w = material index (int bits)
    vec4 v1;
    vec4 v2;
    vec4 n0;
    vec4 n1;
    vec4 n2;
};

struct GPUNode {
    vec4 minLeft;  // xyz = aabb min, w = leftFirst (int bits)
    vec4 maxCount; // xyz = aabb max, w = count (int bits)
};

struct GPUMaterial {
    vec4 albedoMetallic; // rgb = albedo, w = metallic
    vec4 roughness;      // x = roughness, y = transmission, z = ior
    vec4 emissive;       // rgb premultiplied by strength
};

static float IntBits(int v)
{
    float f;
    static_assert(sizeof(f) == sizeof(v));
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

void PathTracer::Init()
{
    m_Compute = std::make_unique<Shader>(std::string(FORGE_ASSET_DIR) + "/shaders/pathtrace.comp");
    m_Atrous = std::make_unique<Shader>(std::string(FORGE_ASSET_DIR) + "/shaders/atrous.comp");
    m_Resolve = std::make_unique<Shader>(std::string(FORGE_ASSET_DIR) + "/shaders/resolve.comp");
    glGenBuffers(1, &m_TriSSBO);
    glGenBuffers(1, &m_NodeSSBO);
    glGenBuffers(1, &m_MatSSBO);
}

void PathTracer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || (width == m_Width && height == m_Height))
        return;
    m_Width = width;
    m_Height = height;

    glDeleteTextures(1, &m_AccumTex);
    glDeleteTextures(1, &m_DisplayTex);
    glDeleteTextures(1, &m_AlbedoTex);
    glDeleteTextures(1, &m_NormalDepthTex);
    glDeleteTextures(1, &m_PingTex);
    glDeleteTextures(1, &m_PongTex);

    auto makeTex = [&](uint32_t& tex, GLenum format) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, format, (GLsizei)width, (GLsizei)height);
    };
    makeTex(m_AccumTex, GL_RGBA32F);
    makeTex(m_AlbedoTex, GL_RGBA16F);
    makeTex(m_NormalDepthTex, GL_RGBA16F);
    makeTex(m_PingTex, GL_RGBA16F);
    makeTex(m_PongTex, GL_RGBA16F);

    makeTex(m_DisplayTex, GL_RGBA8);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    ResetAccumulation();
}

void PathTracer::Upload(const Scene& scene)
{
    std::vector<BVHTriangle> tris;
    std::vector<GPUMaterial> materials;

    for (const Entity& e : scene.Entities()) {
        if (!e.mesh)
            continue;
        // Light gizmo spheres are editor visualization, not scene geometry:
        // tracing them buries the light source inside an occluder and shows a
        // white ball in renders. Standard tools keep lights invisible.
        if (e.light.enabled)
            continue;

        int matIndex = (int)materials.size();
        GPUMaterial gm;
        gm.albedoMetallic = vec4(e.material.albedo, e.material.metallic);
        gm.roughness = vec4(e.material.roughness, e.material.transmission, e.material.ior, 0);
        gm.emissive = vec4(e.material.emissive * e.material.emissiveStrength, 0);
        materials.push_back(gm);

        mat4 world = scene.WorldTransform(e.id);
        mat3 normalMat = mat3(glm::transpose(glm::inverse(world)));

        const auto& verts = e.mesh->Vertices();
        const auto& idx = e.mesh->Indices();
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            BVHTriangle t;
            t.v0 = vec3(world * vec4(verts[idx[i]].position, 1.0f));
            t.v1 = vec3(world * vec4(verts[idx[i + 1]].position, 1.0f));
            t.v2 = vec3(world * vec4(verts[idx[i + 2]].position, 1.0f));
            t.n0 = glm::normalize(normalMat * verts[idx[i]].normal);
            t.n1 = glm::normalize(normalMat * verts[idx[i + 1]].normal);
            t.n2 = glm::normalize(normalMat * verts[idx[i + 2]].normal);
            t.material = matIndex;
            t.centroid = (t.v0 + t.v1 + t.v2) / 3.0f;
            tris.push_back(t);
        }
    }

    // Ground plane: matte studio floor at y=0 (parity with the raster grid's
    // shadow catcher — without it scenes float and lights have nothing to pool on).
    if (m_GroundPlane) {
        int matIndex = (int)materials.size();
        GPUMaterial floorMat;
        floorMat.albedoMetallic = vec4(0.42f, 0.43f, 0.45f, 0.0f);
        floorMat.roughness = vec4(1.0f, 0.0f, 1.5f, 0); // opaque; ior unread when transmission = 0
        floorMat.emissive = vec4(0.0f);
        materials.push_back(floorMat);

        const float kExtent = 300.0f;
        vec3 c[4] = {{-kExtent, 0, -kExtent}, {kExtent, 0, -kExtent}, {kExtent, 0, kExtent}, {-kExtent, 0, kExtent}};
        vec3 up(0, 1, 0);
        auto addTri = [&](const vec3& a, const vec3& b, const vec3& d) {
            BVHTriangle t;
            t.v0 = a; t.v1 = b; t.v2 = d;
            t.n0 = t.n1 = t.n2 = up;
            t.material = matIndex;
            t.centroid = (a + b + d) / 3.0f;
            tris.push_back(t);
        };
        addTri(c[0], c[2], c[1]);
        addTri(c[0], c[3], c[2]);
    }

    BVH bvh;
    bvh.Build(tris);
    m_TriCount = tris.size();
    m_NodeCount = bvh.Nodes().size();

    std::vector<GPUTriangle> gpuTris(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) {
        const BVHTriangle& t = tris[i];
        gpuTris[i] = {vec4(t.v0, IntBits(t.material)), vec4(t.v1, 0), vec4(t.v2, 0),
                      vec4(t.n0, 0), vec4(t.n1, 0), vec4(t.n2, 0)};
    }
    std::vector<GPUNode> gpuNodes(bvh.Nodes().size());
    for (size_t i = 0; i < bvh.Nodes().size(); ++i) {
        const BVHNode& n = bvh.Nodes()[i];
        gpuNodes[i] = {vec4(n.min, IntBits(n.leftFirst)), vec4(n.max, IntBits(n.count))};
    }

    auto upload = [](uint32_t ssbo, const void* data, size_t bytes) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)std::max<size_t>(bytes, 16), bytes ? data : nullptr,
                     GL_STATIC_DRAW);
    };
    upload(m_TriSSBO, gpuTris.data(), gpuTris.size() * sizeof(GPUTriangle));
    upload(m_NodeSSBO, gpuNodes.data(), gpuNodes.size() * sizeof(GPUNode));
    upload(m_MatSSBO, materials.data(), materials.size() * sizeof(GPUMaterial));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    FORGE_INFO("PathTracer: uploaded %zu triangles, %zu BVH nodes", m_TriCount, m_NodeCount);
}

void PathTracer::Dispatch(const mat4& viewProjection, const vec3& cameraPos, const DirectionalLight& sun, int maxBounces,
                          const std::vector<PointLightDraw>& pointLights, const Environment* env, int samplesPerPass)
{
    if (m_Width == 0 || m_Height == 0)
        return;
    samplesPerPass = std::max(samplesPerPass, 1);

    m_Compute->Bind();
    m_Compute->SetMat4("u_InvViewProj", glm::inverse(viewProjection));
    m_Compute->SetVec3("u_CamPos", cameraPos);
    m_Compute->SetInt("u_SampleIndex", m_SampleCount);
    m_Compute->SetInt("u_FrameIndex", m_FrameIndex);
    m_Compute->SetInt("u_SamplesPerPass", samplesPerPass);
    m_Compute->SetInt("u_MaxBounces", maxBounces);
    m_Compute->SetInt("u_NumNodes", (int)m_NodeCount);
    m_Compute->SetFloat("u_Aperture", m_Aperture);
    m_Compute->SetFloat("u_FocusDist", m_FocusDist);
    m_Compute->SetVec3("u_CamRight", m_CamRight);
    m_Compute->SetVec3("u_CamUp", m_CamUp);
    m_Compute->SetVec3("u_SunDir", glm::normalize(sun.direction));
    m_Compute->SetVec3("u_SunColor", sun.color);
    m_Compute->SetFloat("u_SunIntensity", sun.intensity);

    int numLights = std::min((int)pointLights.size(), kMaxPointLights);
    m_Compute->SetInt("u_NumPointLights", numLights);
    for (int i = 0; i < numLights; ++i) {
        std::string idx = "[" + std::to_string(i) + "]";
        m_Compute->SetVec3("u_LightPos" + idx, pointLights[i].position);
        m_Compute->SetVec3("u_LightColor" + idx, pointLights[i].color);
        m_Compute->SetFloat("u_LightRange" + idx, pointLights[i].range);
    }

    bool hasEnv = env && env->Valid();
    m_Compute->SetInt("u_HasEnv", hasEnv ? 1 : 0);
    m_Compute->SetInt("u_EnvMap", 0);
    if (hasEnv) {
        m_Compute->SetFloat("u_EnvIntensity", env->intensity);
        m_Compute->SetFloat("u_EnvRotation", glm::radians(env->rotationDegrees));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, env->Source());
    }

    glBindImageTexture(0, m_AccumTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
    glBindImageTexture(2, m_AlbedoTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(3, m_NormalDepthTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_TriSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_NodeSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_MatSSBO);

    GLuint groupsX = (m_Width + 7) / 8, groupsY = (m_Height + 7) / 8;
    glDispatchCompute(groupsX, groupsY, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    m_SampleCount += samplesPerPass;
    ++m_FrameIndex;

    // --- denoise + resolve ---------------------------------------------------
    // Past ~2048 spp the raw image is cleaner than any filter; skip the passes.
    bool filter = m_Denoise && m_DenoiseStrength > 0.0f && m_SampleCount <= 2048;
    uint32_t filteredTex = m_PingTex;
    if (filter) {
        m_Atrous->Bind();
        m_Atrous->SetFloat("u_Spp", (float)m_SampleCount);
        glBindImageTexture(0, m_AccumTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
        glBindImageTexture(3, m_AlbedoTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        glBindImageTexture(4, m_NormalDepthTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
        uint32_t src = m_PongTex, dst = m_PingTex; // first pass reads accum, src is dummy
        const int steps[4] = {1, 2, 4, 8};
        for (int i = 0; i < 4; ++i) {
            m_Atrous->SetInt("u_StepSize", steps[i]);
            m_Atrous->SetInt("u_FirstPass", i == 0 ? 1 : 0);
            glBindImageTexture(1, src, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
            glBindImageTexture(2, dst, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            glDispatchCompute(groupsX, groupsY, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            std::swap(src, dst);
        }
        filteredTex = src; // last written
    }

    // Blend fades as accumulation converges so fine detail comes back.
    float strength = std::min(m_DenoiseStrength * 8.0f / std::sqrt((float)std::max(m_SampleCount, 1)), 1.0f);
    m_Resolve->Bind();
    m_Resolve->SetFloat("u_Strength", strength);
    m_Resolve->SetInt("u_UseFiltered", filter ? 1 : 0);
    glBindImageTexture(0, m_AccumTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    glBindImageTexture(1, filteredTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(2, m_DisplayTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glDispatchCompute(groupsX, groupsY, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
}

} // namespace forge
