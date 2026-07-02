#include "McpImage.h"

#include <GL/glew.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h> // bundled with tinygltf

#include <cstring>
#include <vector>

namespace forge {

namespace {

// Own tiny encoder rather than httplib::detail::base64_encode — detail:: is
// private API and free to churn between releases.
std::string Base64(const std::vector<uint8_t>& in)
{
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += alphabet[n & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t n = in[i] << 16;
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t n = (in[i] << 16) | (in[i + 1] << 8);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

} // namespace

std::string TextureToPngBase64(uint32_t texture, int maxDim)
{
    if (texture == 0)
        return {};

    glBindTexture(GL_TEXTURE_2D, texture);
    int w = 0, h = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
    if (w <= 0 || h <= 0) {
        glBindTexture(GL_TEXTURE_2D, 0);
        return {};
    }
    std::vector<uint8_t> pixels((size_t)w * h * 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // Flip upright while (optionally) downscaling by nearest sampling.
    int outW = w, outH = h;
    if (maxDim > 0 && (w > maxDim || h > maxDim)) {
        float scale = (float)maxDim / (float)(w > h ? w : h);
        outW = (int)(w * scale);
        outH = (int)(h * scale);
        if (outW < 1) outW = 1;
        if (outH < 1) outH = 1;
    }
    std::vector<uint8_t> upright((size_t)outW * outH * 4);
    for (int y = 0; y < outH; ++y) {
        int srcY = h - 1 - (int)((int64_t)y * h / outH);
        for (int x = 0; x < outW; ++x) {
            int srcX = (int)((int64_t)x * w / outW);
            std::memcpy(&upright[((size_t)y * outW + x) * 4],
                        &pixels[((size_t)srcY * w + srcX) * 4], 4);
        }
    }

    std::vector<uint8_t> png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* buf = (std::vector<uint8_t>*)ctx;
            buf->insert(buf->end(), (uint8_t*)data, (uint8_t*)data + size);
        },
        &png, outW, outH, 4, upright.data(), outW * 4);
    if (png.empty())
        return {};
    return Base64(png);
}

} // namespace forge
