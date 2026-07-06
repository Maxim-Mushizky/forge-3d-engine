#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace forge {

class Texture2D {
public:
    // flipV: OBJ-style textures expect v=0 at the bottom; glTF images load unflipped.
    static std::shared_ptr<Texture2D> FromFile(const std::string& path, bool srgb = true, bool flipV = false);

    // Decodes an image file to an RGBA8 buffer without touching GL (stb_image).
    // Lets callers repack channels (e.g. roughness -> MR) before constructing.
    static bool LoadPixels(const std::string& path, std::vector<uint8_t>& outRgba,
                           uint32_t& outWidth, uint32_t& outHeight, bool flipV = false);

    // channels: 3 (RGB) or 4 (RGBA). srgb: store as sRGB so sampling returns linear.
    Texture2D(const uint8_t* pixels, uint32_t width, uint32_t height, int channels, bool srgb);
    ~Texture2D();

    Texture2D(const Texture2D&) = delete;
    Texture2D& operator=(const Texture2D&) = delete;

    void Bind(uint32_t slot) const;

    uint32_t Width() const { return m_Width; }
    uint32_t Height() const { return m_Height; }

    // Retained CPU copy (always RGBA8). The path tracer rebuilds its texture
    // arrays from this on every scene upload — the GL handle alone isn't
    // enough (layers need resampling to a common size). Costs 4 bytes/texel;
    // acceptable at prop-scene texture counts, revisit if scenes grow (M6).
    const std::vector<uint8_t>& Pixels() const { return m_Pixels; }
    bool Srgb() const { return m_Srgb; }

private:
    uint32_t m_ID = 0;
    uint32_t m_Width = 0, m_Height = 0;
    bool m_Srgb = false;
    std::vector<uint8_t> m_Pixels; // RGBA8, m_Width * m_Height * 4
};

} // namespace forge
