#pragma once

#include "forge/core/Math.h"
#include "forge/renderer/Renderer.h" // DirectionalLight
#include "forge/renderer/Shader.h"
#include "forge/scene/Scene.h"

#include <memory>

namespace forge {

// Progressive GPU path tracer (OpenGL compute). Each Dispatch adds one sample
// per pixel into an RGBA32F accumulation image and writes a tonemapped RGBA8
// display image the viewport shows directly.
// Albedo and metallic-roughness maps are sampled (#113) via two fixed-size
// texture arrays rebuilt from the textures' retained CPU pixels on Upload;
// normal maps are not (no tangents yet — follow-up issue).
class PathTracer {
public:
    void Init();

    void Resize(uint32_t width, uint32_t height); // no-op if unchanged; resets accumulation
    void ResetAccumulation() { m_SampleCount = 0; }

    // Matte floor at y=0 so objects have ground contact/shadows like the raster grid.
    void SetGroundPlane(bool enabled) { m_GroundPlane = enabled; }
    bool GroundPlane() const { return m_GroundPlane; }

    // Thin-lens depth of field. aperture 0 = pinhole (off). right/up are the
    // camera basis vectors (the lens disc lies in the camera plane).
    void SetLens(float aperture, float focusDistance, const vec3& right, const vec3& up)
    {
        m_Aperture = aperture;
        m_FocusDist = focusDistance;
        m_CamRight = right;
        m_CamUp = up;
    }

    // À-trous wavelet denoiser. strength 0..1; the effective blend fades as
    // samples converge (k/sqrt(spp) curve) so detail returns over time.
    void SetDenoise(bool enabled, float strength)
    {
        m_Denoise = enabled;
        m_DenoiseStrength = strength;
    }
    bool Denoise() const { return m_Denoise; }

    // Flatten the scene: world-space triangles + BVH + materials into SSBOs.
    void Upload(const Scene& scene);

    // samplesPerPass: 1 while the camera moves (latency), 4+ while converging.
    void Dispatch(const mat4& viewProjection, const vec3& cameraPos, const DirectionalLight& sun, int maxBounces,
                  const std::vector<PointLightDraw>& pointLights, const Environment* env, int samplesPerPass = 1);

    uint32_t DisplayTexture() const { return m_DisplayTex; }
    uint32_t Width() const { return m_Width; }
    uint32_t Height() const { return m_Height; }
    int SampleCount() const { return m_SampleCount; }
    size_t TriangleCount() const { return m_TriCount; }

private:
    std::unique_ptr<Shader> m_Compute, m_Atrous, m_Resolve;
    uint32_t m_AccumTex = 0, m_DisplayTex = 0;
    uint32_t m_AlbedoTex = 0, m_NormalDepthTex = 0; // denoiser guides (first hit)
    uint32_t m_PingTex = 0, m_PongTex = 0;          // à-trous ping-pong
    uint32_t m_TriSSBO = 0, m_NodeSSBO = 0, m_MatSSBO = 0;
    uint32_t m_UVSSBO = 0;                          // per-triangle UVs, fetched only on hits
    uint32_t m_AlbedoArray = 0, m_MrArray = 0;      // GL_TEXTURE_2D_ARRAY (sRGB / linear)
    uint32_t m_Width = 0, m_Height = 0;
    int m_SampleCount = 0;
    int m_FrameIndex = 0; // never resets: decorrelates noise across accumulation restarts
    bool m_GroundPlane = true;
    float m_Aperture = 0.0f, m_FocusDist = 8.0f;
    vec3 m_CamRight{1.0f, 0.0f, 0.0f}, m_CamUp{0.0f, 1.0f, 0.0f};
    bool m_Denoise = true;
    float m_DenoiseStrength = 0.7f;
    size_t m_TriCount = 0;
    size_t m_NodeCount = 0;
};

} // namespace forge
