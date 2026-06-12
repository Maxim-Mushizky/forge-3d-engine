#include "PostProcess.h"

#include "forge/core/Log.h"

#include <GL/glew.h>

#include <cstdio>
#include <string>
#include <vector>

namespace forge {

static std::string AssetPath(const char* relative)
{
    return std::string(FORGE_ASSET_DIR) + "/" + relative;
}

void PostProcess::Init()
{
    m_Threshold = std::make_unique<Shader>(AssetPath("shaders/fullscreen.vert"), AssetPath("shaders/bloom_threshold.frag"));
    m_Blur = std::make_unique<Shader>(AssetPath("shaders/fullscreen.vert"), AssetPath("shaders/blur.frag"));
    m_Tonemap = std::make_unique<Shader>(AssetPath("shaders/fullscreen.vert"), AssetPath("shaders/tonemap.frag"));
    glGenVertexArrays(1, &m_VAO);
}

void PostProcess::DrawFullscreen() const
{
    glBindVertexArray(m_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void PostProcess::EnsureTargets(uint32_t width, uint32_t height)
{
    if (width == m_Width && height == m_Height)
        return;
    m_Width = width;
    m_Height = height;

    glDeleteFramebuffers(2, m_HalfFBO);
    glDeleteTextures(2, m_HalfTex);
    glDeleteFramebuffers(1, &m_OutFBO);
    glDeleteTextures(1, &m_OutTex);

    uint32_t hw = std::max(width / 2, 1u), hh = std::max(height / 2, 1u);
    glGenFramebuffers(2, m_HalfFBO);
    glGenTextures(2, m_HalfTex);
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, m_HalfTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, (GLsizei)hw, (GLsizei)hh, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, m_HalfFBO[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_HalfTex[i], 0);
    }

    glGenFramebuffers(1, &m_OutFBO);
    glGenTextures(1, &m_OutTex);
    glBindTexture(GL_TEXTURE_2D, m_OutTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, m_OutFBO);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_OutTex, 0);
    FORGE_ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "post framebuffer incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

uint32_t PostProcess::Process(uint32_t hdrTexture, uint32_t width, uint32_t height)
{
    EnsureTargets(width, height);

    glDisable(GL_DEPTH_TEST);
    uint32_t hw = std::max(width / 2, 1u), hh = std::max(height / 2, 1u);

    // 1. Bright-pass into half-res.
    glBindFramebuffer(GL_FRAMEBUFFER, m_HalfFBO[0]);
    glViewport(0, 0, (GLsizei)hw, (GLsizei)hh);
    m_Threshold->Bind();
    m_Threshold->SetInt("u_Src", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdrTexture);
    DrawFullscreen();

    // 2. Separable gaussian blur (two passes per iteration for a wider kernel).
    m_Blur->Bind();
    m_Blur->SetInt("u_Src", 0);
    for (int i = 0; i < 2; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_HalfFBO[1]);
        m_Blur->SetVec3("u_Dir", vec3(1.0f / (float)hw, 0.0f, 0.0f));
        glBindTexture(GL_TEXTURE_2D, m_HalfTex[0]);
        DrawFullscreen();

        glBindFramebuffer(GL_FRAMEBUFFER, m_HalfFBO[0]);
        m_Blur->SetVec3("u_Dir", vec3(0.0f, 1.0f / (float)hh, 0.0f));
        glBindTexture(GL_TEXTURE_2D, m_HalfTex[1]);
        DrawFullscreen();
    }

    // 3. Composite + ACES tonemap + gamma into the LDR output.
    glBindFramebuffer(GL_FRAMEBUFFER, m_OutFBO);
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    m_Tonemap->Bind();
    m_Tonemap->SetInt("u_Scene", 0);
    m_Tonemap->SetInt("u_Bloom", 1);
    m_Tonemap->SetFloat("u_BloomStrength", m_BloomStrength);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hdrTexture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_HalfTex[0]);
    DrawFullscreen();

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_DEPTH_TEST);
    return m_OutTex;
}

} // namespace forge
