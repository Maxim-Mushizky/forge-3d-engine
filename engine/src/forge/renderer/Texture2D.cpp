#include "Texture2D.h"

#include "forge/core/Log.h"

#include <GL/glew.h>

// Implementation lives in ModelImporter.cpp (via TINYGLTF_IMPLEMENTATION).
#include <stb_image.h>

#include <algorithm>

namespace forge {

std::shared_ptr<Texture2D> Texture2D::FromFile(const std::string& path, bool srgb, bool flipV)
{
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!LoadPixels(path, rgba, w, h, flipV))
        return nullptr;
    return std::make_shared<Texture2D>(rgba.data(), w, h, 4, srgb);
}

bool Texture2D::LoadPixels(const std::string& path, std::vector<uint8_t>& outRgba,
                           uint32_t& outWidth, uint32_t& outHeight, bool flipV)
{
    stbi_set_flip_vertically_on_load(flipV ? 1 : 0);
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        FORGE_ERROR("Failed to load texture: %s (%s)", path.c_str(), stbi_failure_reason());
        return false;
    }
    outRgba.assign(pixels, pixels + (size_t)w * h * 4);
    outWidth = (uint32_t)w;
    outHeight = (uint32_t)h;
    stbi_image_free(pixels);
    return true;
}

Texture2D::Texture2D(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, bool srgb)
    : m_Width(width), m_Height(height), m_Srgb(srgb)
{
    // Retain an RGBA8 CPU copy for the path tracer's texture arrays (see header).
    m_Pixels.resize((size_t)width * height * 4);
    if (channels == 4) {
        std::copy(pixels, pixels + m_Pixels.size(), m_Pixels.begin());
    } else {
        for (size_t i = 0, n = (size_t)width * height; i < n; ++i) {
            m_Pixels[i * 4 + 0] = pixels[i * 3 + 0];
            m_Pixels[i * 4 + 1] = pixels[i * 3 + 1];
            m_Pixels[i * 4 + 2] = pixels[i * 3 + 2];
            m_Pixels[i * 4 + 3] = 255;
        }
    }

    GLenum srcFormat = channels == 3 ? GL_RGB : GL_RGBA;
    GLenum internalFormat = srgb ? (channels == 3 ? GL_SRGB8 : GL_SRGB8_ALPHA8)
                                 : (channels == 3 ? GL_RGB8 : GL_RGBA8);

    glGenTextures(1, &m_ID);
    glBindTexture(GL_TEXTURE_2D, m_ID);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, (GLsizei)width, (GLsizei)height, 0, srcFormat, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
}

Texture2D::~Texture2D() { glDeleteTextures(1, &m_ID); }

void Texture2D::Bind(uint32_t slot) const
{
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, m_ID);
}

} // namespace forge
