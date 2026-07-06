#include "TextureGen.h"

#include <json.hpp> // nlohmann, bundled with tinygltf

#include <algorithm>
#include <cmath>
#include <thread>

namespace forge {

using nlohmann::json;

namespace {

// --- hashed-lattice value noise ---------------------------------------------
// Integer-hash noise so a (recipe, seed) pair always bakes identical bytes on
// every platform — no rand(), no float-accumulation order dependence.

uint32_t HashU32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// Lattice value in [0,1). Coordinates wrap at (periodX, periodY) so noise
// built on this tiles exactly.
float LatticeValue(int ix, int iy, int periodX, int periodY, uint32_t seed)
{
    ix = ((ix % periodX) + periodX) % periodX;
    iy = ((iy % periodY) + periodY) % periodY;
    uint32_t h = HashU32((uint32_t)ix * 0x9E3779B1u ^ (uint32_t)iy * 0x85EBCA77u ^
                         seed * 0xC2B2AE3Du);
    return (float)(h >> 8) / 16777216.0f;
}

float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); } // quintic

// x,y in lattice units; smooth bilinear of the four surrounding lattice values.
float ValueNoise(float x, float y, int periodX, int periodY, uint32_t seed)
{
    float fx = std::floor(x), fy = std::floor(y);
    int ix = (int)fx, iy = (int)fy;
    float tx = Fade(x - fx), ty = Fade(y - fy);
    float v00 = LatticeValue(ix, iy, periodX, periodY, seed);
    float v10 = LatticeValue(ix + 1, iy, periodX, periodY, seed);
    float v01 = LatticeValue(ix, iy + 1, periodX, periodY, seed);
    float v11 = LatticeValue(ix + 1, iy + 1, periodX, periodY, seed);
    float a = v00 + (v10 - v00) * tx;
    float b = v01 + (v11 - v01) * tx;
    return a + (b - a) * ty;
}

// Normalized fbm in [0,1]; u,v in [0,1). Octave periods double from `period`,
// staying integers, so the sum keeps the base tile.
float Fbm(float u, float v, int period, int octaves, uint32_t seed)
{
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    int p = period;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * ValueNoise(u * (float)p, v * (float)p, p, p, seed + (uint32_t)o * 101u);
        norm += amp;
        amp *= 0.5f;
        p *= 2;
    }
    return sum / norm;
}

// --- recipe evaluation -------------------------------------------------------

float SrgbEncode(float linear)
{
    linear = std::clamp(linear, 0.0f, 1.0f);
    return linear <= 0.0031308f ? linear * 12.92f
                                : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

// Linear RGB at (u,v) in [0,1). Pure function of the recipe — determinism and
// tileability tests lean on exactly that.
vec3 EvalRecipe(const TextureRecipe& r, float u, float v)
{
    const int cells = std::max(1, (int)std::lround(r.scale));
    switch (r.kind) {
    case TextureKind::Checker: {
        int cu = (int)(u * (float)cells), cv = (int)(v * (float)cells);
        return ((cu + cv) & 1) == 0 ? r.colorA : r.colorB;
    }
    case TextureKind::Stripes: {
        float t = (r.axis == 1 ? v : u) * (float)cells;
        return (t - std::floor(t)) < r.ratio ? r.colorA : r.colorB;
    }
    case TextureKind::Gradient: {
        float t = r.axis == 1 ? v : u;
        return r.colorA + (r.colorB - r.colorA) * t;
    }
    case TextureKind::Noise: {
        float n = Fbm(u, v, cells, r.octaves, r.seed);
        return r.colorA + (r.colorB - r.colorA) * n;
    }
    case TextureKind::Wood: {
        // Concentric growth rings around the texture center, distorted by low
        // frequency fbm ("grainNoise") and shaded by fine anisotropic streaks —
        // reads as a plank/trunk cut for table tops. Not tileable.
        float px = u - 0.5f, py = v - 0.5f;
        float g = Fbm(u, v, 4, r.octaves, r.seed) - 0.5f;
        float rings = std::sqrt(px * px + py * py) * 2.0f * r.scale + r.distort * r.scale * g;
        float band = rings - std::floor(rings);
        float t = std::pow(std::fabs(band * 2.0f - 1.0f), 1.5f);
        vec3 c = r.colorA + (r.colorB - r.colorA) * t;
        // Streaks stretched 16:1 along u (period pair keeps the hash lattice valid).
        float streak = ValueNoise(u * 128.0f, v * 8.0f, 128, 8, r.seed ^ 0x517CC1B7u);
        return c * (0.88f + 0.24f * streak);
    }
    }
    return r.colorA;
}

} // namespace

std::optional<TextureRecipe> RecipeFromJsonText(const std::string& jsonText)
{
    json j = json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
        return std::nullopt;

    TextureRecipe r;
    auto kindIt = j.find("kind");
    if (kindIt == j.end() || !kindIt->is_string())
        return std::nullopt;
    const std::string kind = kindIt->get<std::string>();
    if (kind == "checker")
        r.kind = TextureKind::Checker;
    else if (kind == "stripes")
        r.kind = TextureKind::Stripes;
    else if (kind == "gradient")
        r.kind = TextureKind::Gradient;
    else if (kind == "noise")
        r.kind = TextureKind::Noise;
    else if (kind == "wood")
        r.kind = TextureKind::Wood;
    else
        return std::nullopt; // a typo must not silently bake the default kind

    auto num = [&](const char* key, float fallback) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_number())
            return fallback;
        double d = it->get<double>();
        return std::isfinite(d) ? (float)std::clamp(d, -1e9, 1e9) : fallback;
    };
    auto color = [&](const char* key, vec3 fallback) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_array() || it->size() != 3)
            return fallback;
        vec3 c = fallback;
        for (int i = 0; i < 3; ++i) {
            if (!(*it)[i].is_number())
                return fallback;
            double d = (*it)[i].get<double>();
            if (!std::isfinite(d))
                return fallback;
            c[i] = (float)std::clamp(d, 0.0, 1.0);
        }
        return c;
    };

    r.resolution = ClampResolution((uint32_t)std::max(0.0f, num("resolution", (float)r.resolution)));
    r.seed = (uint32_t)std::max(0.0f, num("seed", 0.0f));
    r.colorA = color("colorA", r.colorA);
    r.colorB = color("colorB", r.colorB);
    r.scale = std::clamp(num("ringScale", num("scale", r.scale)), 0.01f, 1024.0f);
    r.octaves = std::clamp((int)num("octaves", (float)r.octaves), 1, 8);
    r.distort = std::clamp(num("grainNoise", num("distort", r.distort)), 0.0f, 4.0f);
    r.ratio = std::clamp(num("ratio", r.ratio), 0.05f, 0.95f);
    if (auto it = j.find("axis"); it != j.end()) {
        if (it->is_string())
            r.axis = it->get<std::string>() == "v" ? 1 : 0;
        else if (it->is_number_integer())
            r.axis = it->get<int>() == 1 ? 1 : 0;
    }
    return r;
}

std::string RecipeToJsonText(const TextureRecipe& r)
{
    static const char* kKindNames[] = {"checker", "stripes", "gradient", "noise", "wood"};
    json j;
    j["kind"] = kKindNames[(int)r.kind];
    j["resolution"] = r.resolution;
    j["seed"] = r.seed;
    j["colorA"] = json::array({r.colorA.x, r.colorA.y, r.colorA.z});
    j["colorB"] = json::array({r.colorB.x, r.colorB.y, r.colorB.z});
    j["scale"] = r.scale;
    j["octaves"] = r.octaves;
    j["distort"] = r.distort;
    j["ratio"] = r.ratio;
    j["axis"] = r.axis;
    return j.dump();
}

std::vector<uint8_t> BakeTexture(const TextureRecipe& recipe, bool encodeSrgb)
{
    const uint32_t res = ClampResolution(recipe.resolution);
    std::vector<uint8_t> out((size_t)res * res * 4);

    // 2x2 supersampling: hard checker/stripe edges average in LINEAR space, so
    // the mip-less path tracer (textureLod 0) doesn't shimmer on them.
    const float offs[2] = {0.25f, 0.75f};
    const float inv = 1.0f / (float)res;
    auto bakeRows = [&](uint32_t y0, uint32_t y1) {
        for (uint32_t y = y0; y < y1; ++y) {
            for (uint32_t x = 0; x < res; ++x) {
                vec3 c(0.0f);
                for (float oy : offs)
                    for (float ox : offs)
                        c += EvalRecipe(recipe, ((float)x + ox) * inv, ((float)y + oy) * inv);
                c *= 0.25f;
                uint8_t* px = &out[((size_t)y * res + x) * 4];
                for (int i = 0; i < 3; ++i) {
                    float v = encodeSrgb ? SrgbEncode(c[i]) : std::clamp(c[i], 0.0f, 1.0f);
                    px[i] = (uint8_t)std::lround(v * 255.0f);
                }
                px[3] = 255;
            }
        }
    };

    // Every texel is a pure function of the recipe and rows write disjoint
    // ranges, so chunking rows across threads keeps the bytes deterministic
    // while dividing the stall a big bake causes on the GL main thread (MCP
    // handlers run between frames; a serial 4096^2 wood bake takes seconds).
    unsigned threads = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    if (threads <= 1 || res < 256) {
        bakeRows(0, res);
        return out;
    }
    std::vector<std::thread> pool;
    pool.reserve(threads); // a reallocation throw with joinable threads would terminate
    const uint32_t chunk = (res + threads - 1) / threads;
    for (unsigned t = 0; t < threads; ++t) {
        uint32_t y0 = t * chunk, y1 = std::min(res, y0 + chunk);
        if (y0 >= y1)
            break;
        pool.emplace_back(bakeRows, y0, y1);
    }
    for (std::thread& th : pool)
        th.join();
    return out;
}

void PackLuminanceToMR(std::vector<uint8_t>& rgba)
{
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        float lum = 0.2126f * rgba[i] + 0.7152f * rgba[i + 1] + 0.0722f * rgba[i + 2];
        rgba[i + 1] = (uint8_t)std::lround(std::clamp(lum, 0.0f, 255.0f)); // G = roughness
        rgba[i] = 255;     // R unused (glTF reserves it for occlusion)
        rgba[i + 2] = 255; // B = metallic: 255 keeps the material factor
        rgba[i + 3] = 255;
    }
}

std::vector<uint8_t> ResampleRGBA(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                                  uint32_t dstW, uint32_t dstH)
{
    std::vector<uint8_t> out((size_t)dstW * dstH * 4);
    if (!src || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0)
        return out;
    if (srcW == dstW && srcH == dstH) {
        std::copy(src, src + out.size(), out.begin());
        return out;
    }
    for (uint32_t y = 0; y < dstH; ++y) {
        // Texel-center mapping so a 2x downsample averages the right quad.
        float sy = ((float)y + 0.5f) * (float)srcH / (float)dstH - 0.5f;
        int y0 = (int)std::floor(sy);
        float ty = sy - (float)y0;
        int y1 = std::min(std::max(y0 + 1, 0), (int)srcH - 1);
        y0 = std::min(std::max(y0, 0), (int)srcH - 1);
        for (uint32_t x = 0; x < dstW; ++x) {
            float sx = ((float)x + 0.5f) * (float)srcW / (float)dstW - 0.5f;
            int x0 = (int)std::floor(sx);
            float tx = sx - (float)x0;
            int x1 = std::min(std::max(x0 + 1, 0), (int)srcW - 1);
            x0 = std::min(std::max(x0, 0), (int)srcW - 1);
            const uint8_t* p00 = &src[((size_t)y0 * srcW + x0) * 4];
            const uint8_t* p10 = &src[((size_t)y0 * srcW + x1) * 4];
            const uint8_t* p01 = &src[((size_t)y1 * srcW + x0) * 4];
            const uint8_t* p11 = &src[((size_t)y1 * srcW + x1) * 4];
            uint8_t* d = &out[((size_t)y * dstW + x) * 4];
            for (int c = 0; c < 4; ++c) {
                float a = (float)p00[c] + ((float)p10[c] - (float)p00[c]) * tx;
                float b = (float)p01[c] + ((float)p11[c] - (float)p01[c]) * tx;
                d[c] = (uint8_t)std::lround(a + (b - a) * ty);
            }
        }
    }
    return out;
}

} // namespace forge
