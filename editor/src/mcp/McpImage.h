#pragma once

#include <cstdint>
#include <string>

namespace forge {

// Reads an RGBA8 GL texture back, flips it upright (GL is bottom-up), nearest-
// downscales so the longest side fits maxDim, and returns it as a base64 PNG
// ("" on failure). Main-thread only — touches GL. Sized for MCP image content
// blocks: keep maxDim ~1024 so responses stay context-friendly (#76).
std::string TextureToPngBase64(uint32_t texture, int maxDim);

// CPU-buffer variant for software-rendered images (silhouette diffs, #114):
// RGBA8, row 0 = top, no GL. "" on failure.
std::string PixelsToPngBase64(const uint8_t* rgba, int width, int height);

} // namespace forge
