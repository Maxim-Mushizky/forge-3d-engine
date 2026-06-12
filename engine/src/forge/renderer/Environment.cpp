#include "Environment.h"

#include "forge/core/Log.h"
#include "forge/renderer/Shader.h"

#include <GL/glew.h>
#include <stb_image.h>

namespace forge {

static std::string AssetPath(const char* relative)
{
    return std::string(FORGE_ASSET_DIR) + "/" + relative;
}

Environment::~Environment() { Destroy(); }

void Environment::Destroy()
{
    glDeleteTextures(1, &m_Source);
    glDeleteTextures(1, &m_Irradiance);
    glDeleteTextures(1, &m_Prefiltered);
    m_Source = m_Irradiance = m_Prefiltered = 0;
}

bool Environment::Load(const std::string& hdrPath)
{
    stbi_set_flip_vertically_on_load(0);
    int w = 0, h = 0, comp = 0;
    float* pixels = stbi_loadf(hdrPath.c_str(), &w, &h, &comp, 3);
    if (!pixels) {
        FORGE_ERROR("Failed to load HDRI: %s (%s)", hdrPath.c_str(), stbi_failure_reason());
        return false;
    }

    Destroy();
    m_Path = hdrPath;

    glGenTextures(1, &m_Source);
    glBindTexture(GL_TEXTURE_2D, m_Source);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, w, h, 0, GL_RGB, GL_FLOAT, pixels);
    glGenerateMipmap(GL_TEXTURE_2D); // path tracer / prefilter sample lower mips to reduce fireflies
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);        // wraps around horizontally
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // poles clamp
    stbi_image_free(pixels);

    // --- diffuse irradiance (64x32) -----------------------------------------
    constexpr int kIrrW = 64, kIrrH = 32;
    glGenTextures(1, &m_Irradiance);
    glBindTexture(GL_TEXTURE_2D, m_Irradiance);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA16F, kIrrW, kIrrH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    {
        Shader irr(AssetPath("shaders/env_irradiance.comp"));
        irr.Bind();
        irr.SetInt("u_Src", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_Source);
        glBindImageTexture(0, m_Irradiance, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
        glDispatchCompute((kIrrW + 7) / 8, (kIrrH + 7) / 8, 1);
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    }

    // --- prefiltered specular (512x256, 6 mips by roughness) ----------------
    constexpr int kPreW = 512, kPreH = 256, kMips = 6;
    glGenTextures(1, &m_Prefiltered);
    glBindTexture(GL_TEXTURE_2D, m_Prefiltered);
    glTexStorage2D(GL_TEXTURE_2D, kMips, GL_RGBA16F, kPreW, kPreH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    {
        Shader prefilter(AssetPath("shaders/env_prefilter.comp"));
        prefilter.Bind();
        prefilter.SetInt("u_Src", 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_Source);
        for (int mip = 0; mip < kMips; ++mip) {
            prefilter.SetFloat("u_Roughness", (float)mip / (float)(kMips - 1));
            glBindImageTexture(0, m_Prefiltered, mip, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F);
            int mw = std::max(kPreW >> mip, 1), mh = std::max(kPreH >> mip, 1);
            glDispatchCompute((mw + 7) / 8, (mh + 7) / 8, 1);
        }
        glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    }

    FORGE_INFO("Loaded HDRI %s (%dx%d), built IBL maps", hdrPath.c_str(), w, h);
    return true;
}

} // namespace forge
