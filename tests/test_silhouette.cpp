#include "test_framework.h"

#include <forge/assets/MeshBuild.h>
#include <forge/geometry/Silhouette.h>

#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <vector>

// Suites for the silhouette kernel (#114): software rasterization, reference
// binarization, normalization invariances, IoU scoring, and the lathe-cup
// acceptance shape (matching profile >= 0.9 IoU, mismatched clearly lower) —
// all GL-free.

namespace forge::test {

// Vertices already in NDC: an identity MVP makes coverage expectations exact.
static SilhouetteMask RasterNdc(const std::vector<vec3>& positions,
                                const std::vector<uint32_t>& indices, int size)
{
    std::vector<Vertex> verts;
    for (const vec3& p : positions)
        verts.push_back({p, vec3(0.0f, 0.0f, 1.0f), vec2(0.0f)});
    SilhouetteMask mask = MakeMask(size, size);
    RasterizeSilhouette(verts, indices, mat4(1.0f), mask);
    return mask;
}

static void RasterTriangleCoverage()
{
    // NDC triangle (-1,-1) (1,-1) (-1,1) -> pixel-space right triangle whose
    // hypotenuse is the main diagonal (y-down). Centers with y >= x are inside
    // (the diagonal itself is an edge — inclusive), so an 8x8 mask covers
    // 8+7+...+1 = 36 pixels. This pins the edge-function sign convention.
    const SilhouetteMask m = RasterNdc({{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}}, {0, 1, 2}, 8);
    CHECK(MaskArea(m) == 36);
    CHECK(m.pixels[0 * 8 + 0] != 0);  // on-diagonal center counts
    CHECK(m.pixels[7 * 8 + 0] != 0);  // bottom-left corner
    CHECK(m.pixels[0 * 8 + 7] == 0);  // top-right corner is outside
}

static void RasterWindingIndependent()
{
    const SilhouetteMask ccw = RasterNdc({{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}}, {0, 1, 2}, 16);
    const SilhouetteMask cw = RasterNdc({{-1, -1, 0}, {1, -1, 0}, {-1, 1, 0}}, {0, 2, 1}, 16);
    CHECK(MaskArea(ccw) > 0);
    CHECK(ccw.pixels == cw.pixels); // a silhouette has no facing
}

static void RasterQuadWatertight()
{
    // Two triangles sharing the diagonal must tile the full frame: no pinholes
    // along the shared edge (inclusive test double-covers it instead).
    const SilhouetteMask m = RasterNdc({{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}},
                                       {0, 1, 2, 0, 2, 3}, 16);
    CHECK(MaskArea(m) == 16 * 16);
}

static void RasterRejectsDegenerate()
{
    // Zero-area triangle and an out-of-range index both contribute nothing.
    const SilhouetteMask zero =
        RasterNdc({{-1, -1, 0}, {1, 1, 0}, {0, 0, 0}}, {0, 1, 2}, 8);
    CHECK(MaskArea(zero) == 0);
    const SilhouetteMask oob = RasterNdc({{-1, -1, 0}, {1, -1, 0}}, {0, 1, 9}, 8);
    CHECK(MaskArea(oob) == 0);
}

static void ViewProjFraming()
{
    AABB box;
    box.Expand({-2.0f, 0.0f, -0.5f});
    box.Expand({2.0f, 1.0f, 0.5f});

    const std::optional<mat4> front = SilhouetteViewProj("front", box);
    CHECK(front.has_value());
    if (front) {
        // Center projects to NDC origin; the +x/+y corner lands in-frame with
        // x nearly full (larger extent) and y at extent-ratio scale.
        const vec4 c = *front * vec4(0.0f, 0.5f, 0.0f, 1.0f);
        CHECK(ApproxEq(c.x / c.w, 0.0f, 1e-3f));
        CHECK(ApproxEq(c.y / c.w, 0.0f, 1e-3f));
        const vec4 corner = *front * vec4(2.0f, 1.0f, 0.0f, 1.0f);
        CHECK(ApproxEq(corner.x / corner.w, 2.0f / 2.04f, 1e-3f));
        CHECK(ApproxEq(corner.y / corner.w, 0.5f / 2.04f, 1e-3f));
    }

    const std::optional<mat4> top = SilhouetteViewProj("top", box);
    CHECK(top.has_value());
    if (top) {
        // Plan view: world +z points down the image (negative NDC y).
        const vec4 p = *top * vec4(0.0f, 0.5f, 0.5f, 1.0f);
        CHECK(p.y / p.w < -1e-3f);
    }

    CHECK(!SilhouetteViewProj("diagonal", box).has_value());
    CHECK(!SilhouetteViewProj("front", AABB{}).has_value());
}

static void BinarizeAlphaMatte()
{
    // Any meaningful transparency -> alpha IS the matte, colors ignored.
    std::vector<uint8_t> img(4 * 4 * 4, 0);
    auto setPx = [&](int x, int y, uint8_t a) {
        uint8_t* p = &img[((size_t)y * 4 + x) * 4];
        p[0] = p[1] = p[2] = 128;
        p[3] = a;
    };
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            setPx(x, y, 0);
    setPx(1, 1, 255);
    setPx(2, 1, 255);
    const SilhouetteMask m = BinarizeImage(img.data(), 4, 4);
    CHECK(MaskArea(m) == 2);
    CHECK(m.pixels[1 * 4 + 1] != 0);
    CHECK(m.pixels[1 * 4 + 2] != 0);

    // Speck cleanup applies to mattes too: web cutouts carry orphan alpha
    // dust, and one stray pixel would poison the tight-crop bbox.
    const int w = 24, h = 24;
    std::vector<uint8_t> big((size_t)w * h * 4, 0);
    auto setBig = [&](int x, int y, uint8_t a) {
        uint8_t* p = &big[((size_t)y * w + x) * 4];
        p[0] = p[1] = p[2] = 128;
        p[3] = a;
    };
    for (int y = 8; y < 16; ++y)
        for (int x = 8; x < 16; ++x)
            setBig(x, y, 255); // 64-px object
    setBig(1, 22, 255);        // orphan dust: 1 * 20 < 64 -> dropped
    const SilhouetteMask bm = BinarizeImage(big.data(), w, h);
    CHECK(MaskArea(bm) == 64);
    CHECK(bm.pixels[22 * w + 1] == 0);
}

// Otsu fallback exercised for real (the flood path swallows most clean
// cases): a bright fill inside a 1-px dark ring covers >95% of the frame, so
// the flood reports degenerate and the histogram threshold does the split,
// with border-majority polarity picking the fill as figure.
static void BinarizeOtsuFallbackPath()
{
    const int w = 128, h = 128;
    std::vector<uint8_t> img((size_t)w * h * 4);
    auto fill = [&](int x, int y, uint8_t lum) {
        uint8_t* p = &img[((size_t)y * w + x) * 4];
        p[0] = p[1] = p[2] = lum;
        p[3] = 255;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool ring = x == 0 || y == 0 || x == w - 1 || y == h - 1;
            fill(x, y, ring ? 30 : 210);
        }
    const SilhouetteMask m = BinarizeImage(img.data(), w, h);
    CHECK(MaskArea(m) == (w - 2) * (h - 2)); // the bright fill, ring excluded
    CHECK(m.pixels[(size_t)(h / 2) * w + w / 2] != 0);
    CHECK(m.pixels[0] == 0);
}

// Opaque product shot: dark object on a light ground, plus a single dark
// speck. Otsu splits, border majority picks polarity, the speck (under 1/20
// of the object) is dropped so it can't poison the crop box.
static void BinarizeOtsuPolarityAndSpecks()
{
    const int w = 16, h = 16;
    std::vector<uint8_t> img((size_t)w * h * 4);
    auto fill = [&](int x, int y, uint8_t lum) {
        uint8_t* p = &img[((size_t)y * w + x) * 4];
        p[0] = p[1] = p[2] = lum;
        p[3] = 255; // fully opaque: forces the luminance path
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 220);
    for (int y = 4; y < 12; ++y)
        for (int x = 4; x < 12; ++x)
            fill(x, y, 30); // 64-px object
    fill(14, 14, 30); // 1-px speck: 1 * 20 < 64 -> dropped

    const SilhouetteMask dark = BinarizeImage(img.data(), w, h);
    CHECK(MaskArea(dark) == 64);
    CHECK(dark.pixels[5 * w + 5] != 0);
    CHECK(dark.pixels[14 * w + 14] == 0);

    // Inverted polarity: bright object on dark ground still reads as object.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 25);
    for (int y = 4; y < 12; ++y)
        for (int x = 4; x < 12; ++x)
            fill(x, y, 230);
    const SilhouetteMask bright = BinarizeImage(img.data(), w, h);
    CHECK(MaskArea(bright) == 64);
    CHECK(bright.pixels[5 * w + 5] != 0);

    // Featureless image -> nothing to segment -> empty mask, caller errors.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 128);
    CHECK(MaskArea(BinarizeImage(img.data(), w, h)) == 0);
}

// The blue-and-white porcelain case: a white object on a white ground, dark
// decoration inside. A global threshold splits white body from ground WRONG
// (only the decoration survives); the border flood keeps the whole enclosed
// object — outline, white interior, and decoration — as one figure.
static void BinarizeWhiteOnWhite()
{
    const int w = 24, h = 24;
    std::vector<uint8_t> img((size_t)w * h * 4);
    auto fill = [&](int x, int y, uint8_t lum) {
        uint8_t* p = &img[((size_t)y * w + x) * 4];
        p[0] = p[1] = p[2] = lum;
        p[3] = 255;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 250); // white ground
    for (int y = 4; y < 20; ++y)
        for (int x = 4; x < 20; ++x)
            fill(x, y, 245); // white porcelain body, within flood delta of the ground
    for (int y = 4; y < 20; ++y)
        for (int x = 4; x < 20; ++x)
            if (y < 6 || y >= 18 || x < 6 || x >= 18)
                fill(x, y, 120); // shaded object outline seals the flood out
    for (int y = 9; y < 15; ++y)
        for (int x = 9; x < 15; ++x)
            fill(x, y, 40); // cobalt decoration

    const SilhouetteMask m = BinarizeImage(img.data(), w, h);
    CHECK(MaskArea(m) == 16 * 16);       // the WHOLE object, not just the dark bits
    CHECK(m.pixels[10 * w + 7] != 0);    // white interior counted as figure
    CHECK(m.pixels[12 * w + 12] != 0);   // decoration too
    CHECK(m.pixels[2 * w + 2] == 0);     // ground stays ground
}

static SilhouetteMask SolidRect(int maskW, int maskH, int x0, int y0, int w, int h)
{
    SilhouetteMask m = MakeMask(maskW, maskH);
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            m.pixels[(size_t)y * maskW + x] = 255;
    return m;
}

static void NormalizeInvariances()
{
    // Same 2:1 rectangle at a different position and scale, in differently
    // sized frames: canonical forms must match exactly.
    const SilhouetteMask a = NormalizeMask(SolidRect(32, 32, 3, 5, 6, 3), 32);
    const SilhouetteMask b = NormalizeMask(SolidRect(64, 64, 40, 10, 12, 6), 32);
    CHECK(CompareMasks(a, b).iou == 1.0f);

    // Aspect ratio is shape information: a square and a 2:1 rectangle must NOT
    // normalize to the same mask. Square fills 32x32; rect becomes 32x16 -> IoU 0.5.
    const SilhouetteMask sq = NormalizeMask(SolidRect(64, 64, 5, 5, 10, 10), 32);
    const SilhouetteMask rect = NormalizeMask(SolidRect(64, 64, 5, 40, 20, 10), 32);
    CHECK(ApproxEq(CompareMasks(sq, rect).iou, 0.5f, 0.05f));

    // Empty input stays empty (and doesn't crash).
    CHECK(MaskArea(NormalizeMask(MakeMask(16, 16), 32)) == 0);
}

static void CompareAndDiffValues()
{
    SilhouetteMask a = MakeMask(2, 2), b = MakeMask(2, 2);
    a.pixels = {255, 255, 0, 0};
    b.pixels = {255, 0, 255, 0};
    const SilhouetteDiff d = CompareMasks(a, b);
    CHECK(d.intersection == 1);
    CHECK(d.unionArea == 3);
    CHECK(d.onlyA == 1);
    CHECK(d.onlyB == 1);
    CHECK(ApproxEq(d.iou, 1.0f / 3.0f));
    CHECK(ApproxEq(d.dice, 0.5f));

    // Identical, disjoint, mismatched size, and empty-vs-empty.
    CHECK(CompareMasks(a, a).iou == 1.0f);
    SilhouetteMask c = MakeMask(2, 2);
    c.pixels = {0, 0, 0, 255};
    CHECK(CompareMasks(a, c).iou == 0.0f);
    CHECK(CompareMasks(a, MakeMask(3, 3)).iou == 0.0f);
    CHECK(CompareMasks(MakeMask(2, 2), MakeMask(2, 2)).iou == 0.0f); // no shape != a match

    const std::vector<uint8_t> img = DiffImageRGBA(a, b);
    CHECK(img.size() == 2 * 2 * 4);
    CHECK(img[0 * 4] == 225 && img[0 * 4 + 1] == 225); // both -> light gray
    CHECK(img[1 * 4 + 1] == 220);                      // only A -> green channel high
    CHECK(img[2 * 4] == 230 && img[2 * 4 + 2] == 230); // only B -> magenta
    CHECK(img[3 * 4] == 18);                           // neither -> background
}

// Acceptance shape (#114): a lathe cup silhouette matches a moved/uniformly
// scaled copy of itself at IoU >= 0.9, and clearly mismatches a squat variant.
static void LatheCupAcceptance()
{
    const std::vector<vec2> tall = {{0.02f, 0.0f}, {0.35f, 0.0f}, {0.38f, 0.05f},
                                    {0.40f, 0.60f}, {0.42f, 1.00f}};
    const std::vector<vec2> squat = {{0.02f, 0.0f}, {0.50f, 0.0f}, {0.55f, 0.10f},
                                     {0.55f, 0.35f}, {0.50f, 0.50f}};
    MeshData tallCup, squatCup;
    CHECK(BuildLathe(tall, 48, true, tallCup));
    CHECK(BuildLathe(squat, 48, true, squatCup));

    AABB tallBox;
    for (const Vertex& v : tallCup.vertices)
        tallBox.Expand(v.position);
    const std::optional<mat4> vp = SilhouetteViewProj("front", tallBox);
    CHECK(vp.has_value());
    if (!vp)
        return;

    SilhouetteMask base = MakeMask(128, 128);
    RasterizeSilhouette(tallCup.vertices, tallCup.indices, *vp, base);
    CHECK(MaskArea(base) > 0);

    // Same cup, moved and uniformly scaled; fresh framing for its own bounds.
    const mat4 model = glm::scale(
        glm::translate(mat4(1.0f), vec3(3.0f, -1.0f, 2.0f)), vec3(1.7f));
    AABB movedBox;
    for (const Vertex& v : tallCup.vertices)
        movedBox.Expand(vec3(model * vec4(v.position, 1.0f)));
    const std::optional<mat4> vpMoved = SilhouetteViewProj("front", movedBox);
    CHECK(vpMoved.has_value());
    SilhouetteMask moved = MakeMask(128, 128);
    RasterizeSilhouette(tallCup.vertices, tallCup.indices, *vpMoved * model, moved);

    AABB squatBox;
    for (const Vertex& v : squatCup.vertices)
        squatBox.Expand(v.position);
    const std::optional<mat4> vpSquat = SilhouetteViewProj("front", squatBox);
    CHECK(vpSquat.has_value());
    SilhouetteMask squatMask = MakeMask(128, 128);
    RasterizeSilhouette(squatCup.vertices, squatCup.indices, *vpSquat, squatMask);

    const SilhouetteMask n0 = NormalizeMask(base, 128);
    const SilhouetteMask n1 = NormalizeMask(moved, 128);
    const SilhouetteMask n2 = NormalizeMask(squatMask, 128);
    const float match = CompareMasks(n0, n1).iou;
    const float mismatch = CompareMasks(n0, n2).iou;
    CHECK(match >= 0.9f);      // the issue's matching-shape bar
    CHECK(mismatch < 0.75f);   // squat cup reads as a different shape
    CHECK(mismatch < match);
}

void RunSilhouetteTests()
{
    RasterTriangleCoverage();
    RasterWindingIndependent();
    RasterQuadWatertight();
    RasterRejectsDegenerate();
    ViewProjFraming();
    BinarizeAlphaMatte();
    BinarizeOtsuPolarityAndSpecks();
    BinarizeOtsuFallbackPath();
    BinarizeWhiteOnWhite();
    NormalizeInvariances();
    CompareAndDiffValues();
    LatheCupAcceptance();
    std::printf("[ok] silhouette kernel tests done\n");
}

} // namespace forge::test
