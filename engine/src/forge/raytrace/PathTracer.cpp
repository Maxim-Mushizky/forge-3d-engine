#include "PathTracer.h"

#include "forge/core/Log.h"
#include "forge/raytrace/BVH.h"
#include "forge/renderer/TextureGen.h"

#include <GL/glew.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

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
    vec4 tex;            // x = albedo array layer, y = MR array layer; -1 = none (#113)
};

static float IntBits(int v)
{
    float f;
    static_assert(sizeof(f) == sizeof(v));
    std::memcpy(&f, &v, sizeof(f));
    return f;
}

// All layers of a texture array share one size, so every material texture is
// resampled (from its retained CPU pixels) to this. 1024 covers prop-scale
// close-ups; bump alongside a mip strategy if 4K file textures start losing
// visible detail in renders.
static constexpr uint32_t kTexLayerSize = 1024;
// Layer cap per array — beyond it materials fall back to factors with a
// warning rather than failing the upload (GL guarantees >= 256 layers; 64
// keeps the resample cost bounded).
static constexpr int kMaxTexLayers = 64;

// Builds an immutable single-mip array from the textures' retained CPU pixels.
// Empty list -> 1x1 white placeholder so the samplers are always complete.
// textureLod(..., 0) sampling makes mips useless weight here; AA jitter
// converges edge aliasing instead.
static uint32_t BuildTextureArray(const std::vector<const Texture2D*>& textures, bool srgb)
{
    const bool dummy = textures.empty();
    const uint32_t size = dummy ? 1 : kTexLayerSize;
    const int layers = dummy ? 1 : (int)textures.size();

    uint32_t tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, (GLsizei)size,
                   (GLsizei)size, layers);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (dummy) {
        const uint8_t white[4] = {255, 255, 255, 255};
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white);
    } else {
        for (int i = 0; i < layers; ++i) {
            const Texture2D* t = textures[i];
            std::vector<uint8_t> pixels =
                ResampleRGBA(t->Pixels().data(), t->Width(), t->Height(), size, size);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, (GLsizei)size, (GLsizei)size, 1,
                            GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        }
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return tex;
}

void PathTracer::Init()
{
    m_Compute = std::make_unique<Shader>(std::string(FORGE_ASSET_DIR) + "/shaders/pathtrace.comp");
    m_Atrous = std::make_unique<Shader>(std::string(FORGE_ASSET_DIR) + "/shaders/atrous.comp");
    m_Resolve = std::make_unique<Shader>(std::string(FORGE_ASSET_DIR) + "/shaders/resolve.comp");
    glGenBuffers(1, &m_TriSSBO);
    glGenBuffers(1, &m_NodeSSBO);
    glGenBuffers(1, &m_MatSSBO);
    glGenBuffers(1, &m_UVSSBO);
    // Placeholder arrays so Dispatch before the first Upload binds complete textures.
    m_AlbedoArray = BuildTextureArray({}, /*srgb=*/true);
    m_MrArray = BuildTextureArray({}, /*srgb=*/false);
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

    // Texture -> array-layer assignment, deduped across the whole scene (#113).
    // Raw keys are safe: the shared_ptrs on the entities outlive this call.
    std::unordered_map<const Texture2D*, int> albedoLayers, mrLayers;
    std::vector<const Texture2D*> albedoList, mrList;
    bool layerOverflow = false;
    auto layerFor = [&](const std::shared_ptr<Texture2D>& t,
                        std::unordered_map<const Texture2D*, int>& map,
                        std::vector<const Texture2D*>& list) {
        if (!t || t->Pixels().empty())
            return -1;
        auto [it, inserted] = map.try_emplace(t.get(), (int)list.size());
        if (inserted) {
            if ((int)list.size() >= kMaxTexLayers) {
                map.erase(it);
                layerOverflow = true;
                return -1; // factor-only fallback, warned once below
            }
            list.push_back(t.get());
        }
        return it->second;
    };

    for (const Entity& e : scene.Entities()) {
        if (!e.mesh)
            continue;
        // Light gizmo spheres are editor visualization, not scene geometry:
        // tracing them buries the light source inside an occluder and shows a
        // white ball in renders. Standard tools keep lights invisible.
        if (e.light.enabled)
            continue;

        // One GPUMaterial per material slot the mesh actually uses (#80):
        // submesh ranges map to slot materials, whole-buffer meshes to slot 0.
        std::unordered_map<uint32_t, int> slotToIndex;
        auto materialIndexForSlot = [&](uint32_t slot) {
            auto [it, inserted] = slotToIndex.try_emplace(slot, (int)materials.size());
            if (inserted) {
                const Material& m = MaterialForSlot(e, slot);
                GPUMaterial gm;
                gm.albedoMetallic = vec4(m.albedo, m.metallic);
                gm.roughness = vec4(m.roughness, m.transmission, m.ior, 0);
                gm.emissive = vec4(m.emissive * m.emissiveStrength, 0);
                gm.tex = vec4((float)layerFor(m.albedoMap, albedoLayers, albedoList),
                              (float)layerFor(m.metallicRoughnessMap, mrLayers, mrList), 0, 0);
                materials.push_back(gm);
            }
            return it->second;
        };

        mat4 world = scene.WorldTransform(e.id);
        mat3 normalMat = mat3(glm::transpose(glm::inverse(world)));

        const auto& verts = e.mesh->Vertices();
        const auto& idx = e.mesh->Indices();
        auto emitRange = [&](size_t first, size_t count, int matIndex) {
            size_t end = std::min(first + count, idx.size());
            for (size_t i = first; i + 2 < end; i += 3) {
                BVHTriangle t;
                t.v0 = vec3(world * vec4(verts[idx[i]].position, 1.0f));
                t.v1 = vec3(world * vec4(verts[idx[i + 1]].position, 1.0f));
                t.v2 = vec3(world * vec4(verts[idx[i + 2]].position, 1.0f));
                // Skip degenerate (zero-area) triangles — a mesh edit can collapse one,
                // and the shader's normalize(cross(e1,e2)) would yield NaN and render
                // black (#61). A zero-area tri carries no surface, so drop it.
                vec3 faceN = glm::cross(t.v1 - t.v0, t.v2 - t.v0);
                float faceLen = glm::length(faceN);
                if (faceLen < 1e-12f)
                    continue;
                faceN /= faceLen; // geometric normal, used as a fallback below
                // A welded vertex whose adjacent faces cancelled has a ~zero normal;
                // normalize would give NaN, so fall back to the geometric normal.
                auto safeNormal = [&](const vec3& n) {
                    vec3 m = normalMat * n;
                    float l = glm::length(m);
                    return l > 1e-8f ? m / l : faceN;
                };
                t.n0 = safeNormal(verts[idx[i]].normal);
                t.n1 = safeNormal(verts[idx[i + 1]].normal);
                t.n2 = safeNormal(verts[idx[i + 2]].normal);
                t.uv0 = verts[idx[i]].uv;
                t.uv1 = verts[idx[i + 1]].uv;
                t.uv2 = verts[idx[i + 2]].uv;
                t.material = matIndex;
                t.centroid = (t.v0 + t.v1 + t.v2) / 3.0f;
                tris.push_back(t);
            }
        };
        const auto& subs = e.mesh->Submeshes();
        if (subs.empty()) {
            emitRange(0, idx.size(), materialIndexForSlot(0));
        } else {
            for (const Submesh& sm : subs)
                emitRange(sm.firstIndex, sm.indexCount, materialIndexForSlot(sm.materialSlot));
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
        floorMat.tex = vec4(-1.0f, -1.0f, 0, 0); // untextured (0 would alias layer 0)
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
    // UVs live in their own buffer (2 vec4 per tri), fetched only on accepted
    // hits — growing GPUTriangle would drag them through BVH traversal fetches.
    std::vector<vec4> gpuUVs(tris.size() * 2);
    for (size_t i = 0; i < tris.size(); ++i) {
        const BVHTriangle& t = tris[i];
        gpuTris[i] = {vec4(t.v0, IntBits(t.material)), vec4(t.v1, 0), vec4(t.v2, 0),
                      vec4(t.n0, 0), vec4(t.n1, 0), vec4(t.n2, 0)};
        gpuUVs[i * 2 + 0] = vec4(t.uv0.x, t.uv0.y, t.uv1.x, t.uv1.y);
        gpuUVs[i * 2 + 1] = vec4(t.uv2.x, t.uv2.y, 0, 0);
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
    upload(m_UVSSBO, gpuUVs.data(), gpuUVs.size() * sizeof(vec4));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (layerOverflow)
        FORGE_WARN("PathTracer: more than %d unique textures in a channel — extras render "
                   "factor-only", kMaxTexLayers);
    glDeleteTextures(1, &m_AlbedoArray);
    glDeleteTextures(1, &m_MrArray);
    m_AlbedoArray = BuildTextureArray(albedoList, /*srgb=*/true);
    m_MrArray = BuildTextureArray(mrList, /*srgb=*/false);

    FORGE_INFO("PathTracer: uploaded %zu triangles, %zu BVH nodes, %zu+%zu texture layers",
               m_TriCount, m_NodeCount, albedoList.size(), mrList.size());
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

    m_Compute->SetInt("u_AlbedoTexArr", 1);
    m_Compute->SetInt("u_MrTexArr", 2);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_AlbedoArray);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_MrArray);
    glActiveTexture(GL_TEXTURE0);

    glBindImageTexture(0, m_AccumTex, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
    glBindImageTexture(2, m_AlbedoTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindImageTexture(3, m_NormalDepthTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_TriSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_NodeSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_MatSSBO);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_UVSSBO);

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
