#include "McpViews.h"

#include <cmath>

namespace forge {

// Eye placement on the orbit sphere: azimuth 0 looks down -z at the target's
// front (+z side), matching the scene's facing convention.
static vec3 OrbitDir(float azimuthDeg, float elevationDeg)
{
    const float az = glm::radians(azimuthDeg);
    const float el = glm::radians(elevationDeg);
    return {std::sin(az) * std::cos(el), std::sin(el), std::cos(az) * std::cos(el)};
}

std::vector<ViewSpec> BuildViewSpecs(const std::string& preset, const AABB& target)
{
    if (!target.Valid())
        return {};
    const vec3 center = (target.min + target.max) * 0.5f;
    const float radius = std::max(glm::length(target.max - target.min) * 0.5f, 0.05f);
    // Containing a sphere of radius R at fov f needs dist >= R / sin(f/2);
    // anything closer crops off-axis extremities — the exact geometry these
    // views exist to inspect. 5% margin on top.
    const float kFovDeg = 40.0f;
    const float dist = radius / std::sin(glm::radians(kFovDeg * 0.5f)) * 1.05f;

    auto orbit = [&](float azimuthDeg, float elevationDeg, std::string label) {
        ViewSpec s;
        s.eye = center + OrbitDir(azimuthDeg, elevationDeg) * dist;
        s.center = center;
        s.label = std::move(label);
        return s;
    };
    auto top = [&]() {
        ViewSpec s;
        s.eye = center + vec3(0.0f, dist, 0.0f);
        s.center = center;
        s.up = {0.0f, 0.0f, -1.0f}; // plan view: world +z points down the image
        s.ortho = true;
        s.orthoHalf = radius * 1.15f;
        s.label = "top (ortho)";
        return s;
    };

    std::vector<ViewSpec> specs;
    if (preset == "turntable") {
        specs.push_back(orbit(0.0f, 18.0f, "front"));
        specs.push_back(orbit(90.0f, 18.0f, "right"));
        specs.push_back(orbit(180.0f, 18.0f, "back"));
        specs.push_back(orbit(270.0f, 18.0f, "left"));
    } else if (preset == "4up") {
        specs.push_back(orbit(0.0f, 10.0f, "front"));
        specs.push_back(orbit(90.0f, 10.0f, "right"));
        specs.push_back(top());
        specs.push_back(orbit(45.0f, 28.0f, "three-quarter"));
    } else if (preset == "top_ortho") {
        specs.push_back(top());
    }
    return specs;
}

mat4 ViewProjFor(const ViewSpec& spec, float aspect)
{
    const float dist = glm::length(spec.eye - spec.center);
    const mat4 view = glm::lookAt(spec.eye, spec.center, spec.up);
    if (spec.ortho) {
        const float h = spec.orthoHalf;
        return glm::ortho(-h * aspect, h * aspect, -h, h, dist * 0.01f, dist * 4.0f) * view;
    }
    return glm::perspective(glm::radians(spec.fovDeg), aspect, dist * 0.01f, dist * 10.0f) * view;
}

vec3 IdColor(size_t index)
{
    const float h = std::fmod((float)index * 0.61803398875f, 1.0f) * 6.0f;
    // Hue alone runs out of perceptual distance past ~16 entities (golden-
    // ratio near-misses like index 0 vs 21 differ by ~8 degrees), so every
    // 16 indices step to a different saturation/value tier. Distinct through
    // ~48 entities; beyond that the legend's exact hex still disambiguates.
    // Every tier keeps chroma (s*v) >= ~0.5: pastel tiers compress hue
    // distance and near-golden-ratio hue pairs fall below legibility.
    static constexpr float kS[3] = {0.72f, 0.92f, 0.55f};
    static constexpr float kV[3] = {0.95f, 0.62f, 0.90f};
    const size_t tier = (index / 16) % 3;
    const float s = kS[tier], v = kV[tier];
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
    const float m = v - c;
    vec3 rgb = h < 1.0f ? vec3(c, x, 0.0f)
             : h < 2.0f ? vec3(x, c, 0.0f)
             : h < 3.0f ? vec3(0.0f, c, x)
             : h < 4.0f ? vec3(0.0f, x, c)
             : h < 5.0f ? vec3(x, 0.0f, c)
                        : vec3(c, 0.0f, x);
    return rgb + vec3(m);
}

} // namespace forge
