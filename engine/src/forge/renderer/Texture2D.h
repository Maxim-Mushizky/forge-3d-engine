#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace forge {

class Texture2D {
public:
    // flipV: OBJ-style textures expect v=0 at the bottom; glTF images load unflipped.
    static std::shared_ptr<Texture2D> FromFile(const std::string& path, bool srgb = true, bool flipV = false);

    // channels: 3 (RGB) or 4 (RGBA). srgb: store as sRGB so sampling returns linear.
    Texture2D(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, bool srgb);
    ~Texture2D();

    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;

    void Bind(uint32_t slot) const;

    uint32_t Width() const { return m_Width; }
    uint32_t Height() const { return m_Height; }

private:
    uint32_t m_ID = 0;
    uint32_t m_Width = 0, m_Height = 0;
};

} // namespace forge
