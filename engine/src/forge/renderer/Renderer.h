#pragma once

#include "forge/core/Geometry.h"
#include "forge/core/Math.h"
#include "forge/renderer/Environment.h"
#include "forge/renderer/Framebuffer.h"
#include "forge/renderer/Material.h"
#include "forge/renderer/Mesh.h"
#include "forge/renderer/Shader.h"

#include <memory>
#include <optional>
#include <vector>

namespace forge {

struct DirectionalLight {
    vec3 direction{-0.5f, -1.0f, -0.3f};
    vec3 color{1.0f, 1.0f, 0.95f};
    float intensity = 1.0f;
};

struct PointLightDraw {
    vec3 position;
    vec3 color; // premultiplied by intensity
    float range;
};

enum class ShadingMode {
    Flat,       // unlit albedo
    BlinnPhong,
    PBR,        // Cook-Torrance metallic/roughness + IBL
};

inline constexpr int kMaxPointLights = 8;

// The only class that issues draw calls (raster path).
// Submissions are queued so EndScene can render them twice:
// shadow-depth pass into the light's framebuffer, then the main pass.
class Renderer {
public:
    void Init();

    void SetShadingMode(ShadingMode mode) { m_Mode = mode; }
    ShadingMode GetShadingMode() const { return m_Mode; }
    void SetShadowsEnabled(bool enabled) { m_ShadowsEnabled = enabled; }
    bool ShadowsEnabled() const { return m_ShadowsEnabled; }
    void SetEnvironment(const Environment* env) { m_Environment = env; } // null = procedural background

    void BeginScene(const mat4& viewProjection, const vec3& cameraPosition, const DirectionalLight& light);
    void Submit(const Mesh& mesh, const mat4& transform, const Material& material, bool castShadow = true);
    void SubmitLight(const vec3& position, const vec3& color, float intensity, float range);
    void SetOutline(const Mesh& mesh, const mat4& transform); // selection wireframe
    void EndScene(const Framebuffer& target);                 // leaves `target` bound

private:
    struct DrawItem {
        const Mesh* mesh;
        mat4 transform;
        Material material;
        bool castShadow;
    };

    AABB SceneBounds() const;
    void ShadowPass();
    void SkyPass();
    void DrawItemMain(const DrawItem& item);

    std::unique_ptr<Shader> m_BlinnPhong;
    std::unique_ptr<Shader> m_Flat;
    std::unique_ptr<Shader> m_PBR;
    std::unique_ptr<Shader> m_GridShader;
    std::unique_ptr<Shader> m_ShadowDepth;
    std::unique_ptr<Shader> m_Sky;
    std::shared_ptr<Mesh> m_GridPlane;
    std::shared_ptr<Mesh> m_SkyDome;

    static constexpr uint32_t kShadowResolution = 2048;
    uint32_t m_ShadowFBO = 0, m_ShadowTex = 0;
    mat4 m_LightSpace{1.0f};
    bool m_ShadowsEnabled = true;
    bool m_ShadowsThisFrame = false;

    ShadingMode m_Mode = ShadingMode::PBR;
    mat4 m_ViewProjection{1.0f};
    vec3 m_CameraPosition{0.0f};
    DirectionalLight m_Light;
    const Environment* m_Environment = nullptr;

    std::vector<DrawItem> m_Queue;
    std::vector<PointLightDraw> m_PointLights;
    std::optional<std::pair<const Mesh*, mat4>> m_Outline;
};

} // namespace forge
