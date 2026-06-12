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
// Limitation (v1): material textures are not sampled — factors only.
class PathTracer {
public:
    void Init();

    void Resize(uint32_t width, uint32_t height); // no-op if unchanged; resets accumulation
    void ResetAccumulation() { m_SampleCount = 0; }

    // Matte floor at y=0 so objects have ground contact/shadows like the raster grid.
    void SetGroundPlane(bool enabled) { m_GroundPlane = enabled; }
    bool GroundPlane() const { return m_GroundPlane; }

    // Flatten the scene: world-space triangles + BVH + materials into SSBOs.
    void Upload(const Scene& scene);

    // samplesPerPass: 1 while the camera moves (latency), 4+ while converging.
    void Dispatch(const mat4& viewProjection, const vec3& cameraPos, const DirectionalLight& sun, int maxBounces,
                  const std::vector<PointLightDraw>& pointLights, const Environment* env, int samplesPerPass = 1);

    uint32_t DisplayTexture() const { return m_DisplayTex; }
    int SampleCount() const { return m_SampleCount; }
    size_t TriangleCount() const { return m_TriCount; }

private:
    std::unique_ptr<Shader> m_Compute;
    uint32_t m_AccumTex = 0, m_DisplayTex = 0;
    uint32_t m_TriSSBO = 0, m_NodeSSBO = 0, m_MatSSBO = 0;
    uint32_t m_Width = 0, m_Height = 0;
    int m_SampleCount = 0;
    int m_FrameIndex = 0; // never resets: decorrelates noise across accumulation restarts
    bool m_GroundPlane = true;
    size_t m_TriCount = 0;
    size_t m_NodeCount = 0;
};

} // namespace forge
