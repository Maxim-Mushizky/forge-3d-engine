#pragma once

#include "forge/renderer/Shader.h"

#include <cstdint>
#include <memory>

namespace forge {

// HDR post stack: bloom (threshold -> half-res gaussian) + ACES tonemap + gamma.
// Input: linear RGBA16F scene texture. Output: display-ready RGBA8 texture.
class PostProcess {
public:
    void Init();

    uint32_t Process(uint32_t hdrTexture, uint32_t width, uint32_t height);

    void SetBloomStrength(float strength) { m_BloomStrength = strength; }
    float BloomStrength() const { return m_BloomStrength; }

private:
    void EnsureTargets(uint32_t width, uint32_t height);
    void DrawFullscreen() const;

    std::unique_ptr<Shader> m_Threshold;
    std::unique_ptr<Shader> m_Blur;
    std::unique_ptr<Shader> m_Tonemap;

    uint32_t m_VAO = 0; // empty VAO for the fullscreen triangle
    uint32_t m_HalfFBO[2] = {0, 0}, m_HalfTex[2] = {0, 0}; // ping-pong, half res, RGBA16F
    uint32_t m_OutFBO = 0, m_OutTex = 0;                   // full res, RGBA8
    uint32_t m_Width = 0, m_Height = 0;
    float m_BloomStrength = 0.06f;
};

} // namespace forge
