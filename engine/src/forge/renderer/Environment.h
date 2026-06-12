#pragma once

#include <cstdint>
#include <string>

namespace forge {

// HDRI environment, equirect-based (no cubemap plumbing — pole distortion is
// acceptable at this engine's scale):
//  - source:      full-res equirect HDR (skybox + path-tracer miss shader)
//  - irradiance:  64x32 cosine-convolved equirect (diffuse IBL)
//  - prefiltered: 512x256, 6 mips GGX-filtered by roughness (specular IBL/reflections)
class Environment {
public:
    ~Environment();

    bool Load(const std::string& hdrPath); // builds all maps; false on failure
    bool Valid() const { return m_Source != 0; }

    uint32_t Source() const { return m_Source; }
    uint32_t Irradiance() const { return m_Irradiance; }
    uint32_t Prefiltered() const { return m_Prefiltered; }
    const std::string& Path() const { return m_Path; }

    float intensity = 1.0f;
    float rotationDegrees = 0.0f;

private:
    void Destroy();

    uint32_t m_Source = 0, m_Irradiance = 0, m_Prefiltered = 0;
    std::string m_Path;
};

} // namespace forge
