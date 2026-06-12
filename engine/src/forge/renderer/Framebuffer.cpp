#include "Framebuffer.h"

#include "forge/core/Log.h"

#include <GL/glew.h>

namespace forge {

Framebuffer::Framebuffer(uint32_t width, uint32_t height, bool hdr)
    : m_Width(width), m_Height(height), m_HDR(hdr)
{
    Invalidate();
}

Framebuffer::~Framebuffer() { Destroy(); }

void Framebuffer::Destroy()
{
    glDeleteFramebuffers(1, &m_FBO);
    glDeleteTextures(1, &m_ColorTex);
    glDeleteRenderbuffers(1, &m_DepthRBO);
}

void Framebuffer::Invalidate()
{
    glGenFramebuffers(1, &m_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);

    glGenTextures(1, &m_ColorTex);
    glBindTexture(GL_TEXTURE_2D, m_ColorTex);
    glTexImage2D(GL_TEXTURE_2D, 0, m_HDR ? GL_RGBA16F : GL_RGBA8, (GLsizei)m_Width, (GLsizei)m_Height, 0,
                 GL_RGBA, m_HDR ? GL_FLOAT : GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ColorTex, 0);

    glGenRenderbuffers(1, &m_DepthRBO);
    glBindRenderbuffer(GL_RENDERBUFFER, m_DepthRBO);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)m_Width, (GLsizei)m_Height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_DepthRBO);

    FORGE_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "framebuffer incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Framebuffer::Resize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0 || (width == m_Width && height == m_Height))
        return;
    m_Width = width;
    m_Height = height;
    Destroy();
    Invalidate();
}

void Framebuffer::Bind() const
{
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glViewport(0, 0, (GLsizei)m_Width, (GLsizei)m_Height);
}

void Framebuffer::Unbind() const { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

} // namespace forge
