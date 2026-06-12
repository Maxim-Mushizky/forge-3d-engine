#pragma once

#include <cstdint>

namespace forge {

// Off-screen render target: RGBA8 color + depth. The editor viewport
// displays the color attachment as an ImGui image.
class Framebuffer {
public:
    // hdr: RGBA16F color attachment (for linear HDR rendering before the post stack)
    Framebuffer(uint32_t width, uint32_t height, bool hdr = false);
    ~Framebuffer();

    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    void Bind() const;
    void Unbind() const;
    void Resize(uint32_t width, uint32_t height);

    uint32_t ColorAttachment() const { return m_ColorTex; }
    uint32_t Width() const { return m_Width; }
    uint32_t Height() const { return m_Height; }

private:
    void Invalidate();
    void Destroy();

    uint32_t m_FBO = 0, m_ColorTex = 0, m_DepthRBO = 0;
    uint32_t m_Width, m_Height;
    bool m_HDR = false;
};

} // namespace forge
