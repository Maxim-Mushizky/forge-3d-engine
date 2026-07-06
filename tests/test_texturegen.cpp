#include "test_framework.h"

#include "forge/renderer/TextureGen.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Procedural texture baker (#113). The contract the engine relies on:
// deterministic bytes for a fixed recipe (recipes persist in .forge files and
// must rebuild identically), exact colors where a kind promises them, seam
// continuity where tileability is promised, and the parser rejecting typo'd
// kinds instead of silently baking the default.

namespace forge::test {
namespace {

// Texel fetch helper: R channel at (x, y).
uint8_t R(const std::vector<uint8_t>& img, uint32_t res, uint32_t x, uint32_t y)
{
    return img[((size_t)y * res + x) * 4];
}

void TestBakeDeterministic()
{
    TextureRecipe r;
    r.kind = TextureKind::Wood;
    r.resolution = 64;
    r.seed = 7;
    auto a = BakeTexture(r, false);
    auto b = BakeTexture(r, false);
    CHECK(a.size() == 64u * 64u * 4u);
    CHECK(a == b);

    r.seed = 8;
    auto c = BakeTexture(r, false);
    CHECK(a != c);

    // >= 256 takes the multithreaded bake path — bytes must stay deterministic.
    r.resolution = 256;
    r.kind = TextureKind::Noise;
    CHECK(BakeTexture(r, false) == BakeTexture(r, false));

    CHECK(ClampResolution(8) == 16 && ClampResolution(100000) == 4096 &&
          ClampResolution(512) == 512);
}

void TestCheckerExactColors()
{
    TextureRecipe r;
    r.kind = TextureKind::Checker;
    r.resolution = 64;
    r.scale = 4.0f; // 16-texel cells
    r.colorA = {1.0f, 0.0f, 0.0f};
    r.colorB = {0.0f, 0.0f, 1.0f};
    auto img = BakeTexture(r, false);

    // Cell centers are away from edges, so supersampling can't blend them.
    const uint8_t* a = &img[((size_t)8 * 64 + 8) * 4];  // cell (0,0) -> colorA
    const uint8_t* b = &img[((size_t)8 * 64 + 24) * 4]; // cell (1,0) -> colorB
    CHECK(a[0] == 255 && a[1] == 0 && a[2] == 0 && a[3] == 255);
    CHECK(b[0] == 0 && b[1] == 0 && b[2] == 255 && b[3] == 255);
    // Diagonal neighbour shares the color.
    const uint8_t* d = &img[((size_t)24 * 64 + 24) * 4]; // cell (1,1) -> colorA
    CHECK(d[0] == 255 && d[2] == 0);
}

void TestStripesCountRatioAndAxis()
{
    TextureRecipe r;
    r.kind = TextureKind::Stripes;
    r.resolution = 128;
    r.scale = 8.0f; // 16-texel period
    r.ratio = 0.5f;
    r.colorA = {1.0f, 1.0f, 1.0f};
    r.colorB = {0.0f, 0.0f, 0.0f};
    auto img = BakeTexture(r, false);

    // Half the row is colorA (up to one blended texel per edge, 16 edges).
    int bright = 0, transitions = 0;
    for (uint32_t x = 0; x < 128; ++x) {
        if (R(img, 128, x, 64) > 200)
            ++bright;
        if (x > 0 && (R(img, 128, x, 64) > 128) != (R(img, 128, x - 1, 64) > 128))
            ++transitions;
    }
    CHECK(bright >= 56 && bright <= 72);
    CHECK(transitions >= 14 && transitions <= 17); // 8 pairs ~= 15-16 edges in-row
    // Vertical uniformity along the stripe direction.
    CHECK(R(img, 128, 3, 10) == R(img, 128, 3, 100));

    // ratio shifts the balance; axis 'v' turns stripes horizontal.
    r.ratio = 0.25f;
    auto thin = BakeTexture(r, false);
    int thinBright = 0;
    for (uint32_t x = 0; x < 128; ++x)
        if (R(thin, 128, x, 64) > 200)
            ++thinBright;
    CHECK(thinBright >= 24 && thinBright <= 40);

    r.ratio = 0.5f;
    r.axis = 1;
    auto vert = BakeTexture(r, false);
    CHECK(R(vert, 128, 10, 3) == R(vert, 128, 100, 3)); // uniform along u now
}

void TestGradientEndpointsAndMonotonic()
{
    TextureRecipe r;
    r.kind = TextureKind::Gradient;
    r.resolution = 64;
    r.colorA = {0.0f, 0.0f, 0.0f};
    r.colorB = {1.0f, 1.0f, 1.0f};
    auto img = BakeTexture(r, false);

    CHECK(R(img, 64, 0, 32) <= 4);
    CHECK(R(img, 64, 63, 32) >= 251);
    for (uint32_t x = 1; x < 64; ++x)
        CHECK(R(img, 64, x, 32) + 1 >= R(img, 64, x - 1, 32)); // non-decreasing (+-1 rounding)
}

void TestNoiseTilesAndVaries()
{
    TextureRecipe r;
    r.kind = TextureKind::Noise;
    r.resolution = 128;
    r.scale = 4.0f;
    r.octaves = 4;
    r.seed = 11;
    r.colorA = {0.0f, 0.0f, 0.0f};
    r.colorB = {1.0f, 1.0f, 1.0f};
    auto img = BakeTexture(r, false);

    // Tileable: the wrap seam (last -> first column) must look like any other
    // adjacent-texel step, not a cliff.
    int interiorMax = 0, seamMax = 0;
    for (uint32_t y = 0; y < 128; ++y) {
        seamMax = std::max(seamMax, std::abs((int)R(img, 128, 0, y) - (int)R(img, 128, 127, y)));
        for (uint32_t x = 1; x < 128; ++x)
            interiorMax =
                std::max(interiorMax, std::abs((int)R(img, 128, x, y) - (int)R(img, 128, x - 1, y)));
    }
    CHECK(seamMax <= interiorMax + 2);

    // Actually varies, and octaves change the picture.
    int lo = 255, hi = 0;
    for (uint32_t i = 0; i < 128u * 128u; ++i) {
        lo = std::min(lo, (int)img[i * 4]);
        hi = std::max(hi, (int)img[i * 4]);
    }
    CHECK(hi - lo > 40);

    r.octaves = 1;
    CHECK(BakeTexture(r, false) != img);
}

void TestWoodRings()
{
    TextureRecipe r;
    r.kind = TextureKind::Wood;
    r.resolution = 128;
    r.scale = 6.0f; // rings from center to edge
    r.seed = 3;
    r.colorA = {0.0f, 0.0f, 0.0f};
    r.colorB = {1.0f, 1.0f, 1.0f};
    auto img = BakeTexture(r, false);

    // Walking outward from the center must cross ring bands repeatedly.
    int crossings = 0;
    for (uint32_t x = 65; x < 128; ++x)
        if ((R(img, 128, x, 64) > 128) != (R(img, 128, x - 1, 64) > 128))
            ++crossings;
    CHECK(crossings >= 4 && crossings <= 40);

    // grainNoise distortion changes the bake.
    TextureRecipe straight = r;
    straight.distort = 0.0f;
    CHECK(BakeTexture(straight, false) != img);
}

void TestRecipeParseClampsAndAliases()
{
    auto r = RecipeFromJsonText(R"({"kind":"wood","resolution":8,"ringScale":12.5,
        "grainNoise":9,"octaves":20,"ratio":0.001,"seed":42,
        "colorA":[0.2,0.3,0.4],"colorB":[2.0,-1.0,0.5],"axis":"v"})");
    CHECK(r.has_value());
    if (!r)
        return;
    CHECK(r->kind == TextureKind::Wood);
    CHECK(r->resolution == 16);               // clamped up
    CHECK(ApproxEq(r->scale, 12.5f));         // ringScale alias
    CHECK(ApproxEq(r->distort, 4.0f));        // grainNoise alias, clamped
    CHECK(r->octaves == 8);                   // clamped
    CHECK(ApproxEq(r->ratio, 0.05f));         // clamped
    CHECK(r->seed == 42);
    CHECK(ApproxEq(r->colorA.y, 0.3f));
    CHECK(ApproxEq(r->colorB.x, 1.0f) && ApproxEq(r->colorB.y, 0.0f)); // color clamp 0-1
    CHECK(r->axis == 1);

    CHECK(RecipeFromJsonText(R"({"kind":"marble"})") == std::nullopt); // unknown kind
    CHECK(RecipeFromJsonText(R"({"scale":4})") == std::nullopt);      // missing kind
    CHECK(RecipeFromJsonText("[1,2,3]") == std::nullopt);
    CHECK(RecipeFromJsonText("not json") == std::nullopt);

    auto big = RecipeFromJsonText(R"({"kind":"checker","resolution":100000})");
    CHECK(big && big->resolution == 4096);

    // Canonical round-trip: what set_texture persists must re-parse identically.
    auto again = RecipeFromJsonText(RecipeToJsonText(*r));
    CHECK(again.has_value());
    if (again) {
        CHECK(again->kind == r->kind && again->resolution == r->resolution &&
              again->seed == r->seed && again->axis == r->axis);
        CHECK(ApproxEq(again->scale, r->scale) && ApproxEq(again->distort, r->distort) &&
              ApproxEq(again->ratio, r->ratio) && again->octaves == r->octaves);
        CHECK(ApproxEq(again->colorA.x, r->colorA.x) && ApproxEq(again->colorB.z, r->colorB.z));
    }
}

void TestSrgbEncode()
{
    TextureRecipe r;
    r.kind = TextureKind::Gradient;
    r.resolution = 16;
    r.colorA = r.colorB = {0.5f, 0.5f, 0.5f}; // flat mid-gray
    auto linear = BakeTexture(r, false);
    auto srgb = BakeTexture(r, true);
    CHECK(linear[0] == 127 || linear[0] == 128);
    CHECK(srgb[0] >= 186 && srgb[0] <= 189); // sRGB(0.5) ~= 0.7354
}

void TestPackLuminanceToMR()
{
    std::vector<uint8_t> px = {
        255, 255, 255, 255, // white -> roughness 255
        0,   0,   0,   255, // black -> roughness 0
        255, 0,   0,   255, // red -> 0.2126 * 255 ~= 54
    };
    PackLuminanceToMR(px);
    CHECK(px[1] == 255 && px[0] == 255 && px[2] == 255);
    CHECK(px[5] == 0 && px[6] == 255); // B stays 255 so the metallic factor passes through
    CHECK(px[9] == 54 && px[8] == 255 && px[10] == 255);
}

void TestResampleRGBA()
{
    // Identity.
    std::vector<uint8_t> src = {10, 20, 30, 40, 50, 60, 70, 80};
    auto same = ResampleRGBA(src.data(), 2, 1, 2, 1);
    CHECK(same == src);

    // 2x2 -> 1x1 averages the quad (texel-center mapping).
    std::vector<uint8_t> quad(16, 0);
    quad[0] = 0;
    quad[4] = 100;
    quad[8] = 200;
    quad[12] = 56;
    auto one = ResampleRGBA(quad.data(), 2, 2, 1, 1);
    CHECK(one.size() == 4);
    CHECK(one[0] == 89); // (0 + 100 + 200 + 56) / 4

    // 2x1 -> 4x1 interpolates with clamped edges.
    std::vector<uint8_t> two = {0, 0, 0, 255, 200, 0, 0, 255};
    auto four = ResampleRGBA(two.data(), 2, 1, 4, 1);
    CHECK(four[0] == 0 && four[4] == 50 && four[8] == 150 && four[12] == 200);

    // Degenerate dims return a zeroed buffer instead of crashing.
    auto none = ResampleRGBA(nullptr, 0, 0, 2, 2);
    CHECK(none.size() == 16);
}

} // namespace

void RunTextureGenTests()
{
    TestBakeDeterministic();
    TestCheckerExactColors();
    TestStripesCountRatioAndAxis();
    TestGradientEndpointsAndMonotonic();
    TestNoiseTilesAndVaries();
    TestWoodRings();
    TestRecipeParseClampsAndAliases();
    TestSrgbEncode();
    TestPackLuminanceToMR();
    TestResampleRGBA();
}

} // namespace forge::test
