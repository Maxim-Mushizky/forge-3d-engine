#include "test_framework.h"

#include <forge/assets/MeshBuild.h>
#include <forge/geometry/Silhouette.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
            // Three grays (30 / 200 / 210), not two: a strict two-valued gray
            // image would take the #135 fast path and never reach Otsu, so the
            // bright fill carries two tones. The flood still degenerates (>95%
            // fill) into the histogram split this test is here to pin.
            fill(x, y, ring ? 30 : (x < w / 2 ? 200 : 210));
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
    // Two dark object tones keep the image >= 3-valued (30 / 45 / 220): a strict
    // two-valued gray one would take the #135 fast path instead of the border
    // flood + majority polarity this test pins. The split is inside the figure,
    // so both tones stay foreground.
    for (int y = 4; y < 12; ++y)
        for (int x = 4; x < 12; ++x)
            fill(x, y, x < 8 ? 30 : 45); // 64-px object
    fill(14, 14, 30);                    // 1-px speck: 1 * 20 < 64 -> dropped

    const SilhouetteMask dark = BinarizeImage(img.data(), w, h);
    CHECK(MaskArea(dark) == 64);
    CHECK(dark.pixels[5 * w + 5] != 0);
    CHECK(dark.pixels[14 * w + 14] == 0);

    // Inverted polarity: bright object on dark ground still reads as object.
    // Two bright tones again keep it >= 3-valued (25 / 215 / 230).
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 25);
    for (int y = 4; y < 12; ++y)
        for (int x = 4; x < 12; ++x)
            fill(x, y, x < 8 ? 230 : 215);
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

// Enclosed through-hole recovery (#134): backdrop seen THROUGH the object (a
// donut's hole) never connects to the frame border, so the flood alone leaves
// it foreground. A background-toned, sharp-rimmed, fully enclosed region now
// punches back out.
static void BinarizeDonutHoleRecovered()
{
    const int w = 32, h = 32;
    std::vector<uint8_t> img((size_t)w * h * 4);
    auto fill = [&](int x, int y, uint8_t lum) {
        uint8_t* p = &img[((size_t)y * w + x) * 4];
        p[0] = p[1] = p[2] = lum;
        p[3] = 255;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 220); // ground
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x)
            fill(x, y, 60); // 16x16 object
    for (int y = 12; y < 20; ++y)
        for (int x = 12; x < 20; ++x)
            fill(x, y, 160); // hole: the ground occluded and shadowed —
                             // inside the [med/2, med*4/5] window, rim sharp

    const SilhouetteMask m = BinarizeImage(img.data(), w, h);
    CHECK(MaskArea(m) == 16 * 16 - 8 * 8); // the ring, hole carved out
    CHECK(m.pixels[9 * w + 9] != 0);       // ring is figure
    CHECK(m.pixels[15 * w + 15] == 0);     // hole is ground again
    CHECK(m.pixels[2 * w + 2] == 0);       // outer ground untouched
}

// A tone below HALF the ground brightness is object material, not backdrop
// seen through a hole — real backdrops keep most of their light even
// shadowed. The recess must survive recovery.
static void BinarizeDarkRecessKept()
{
    const int w = 32, h = 32;
    std::vector<uint8_t> img((size_t)w * h * 4);
    auto fill = [&](int x, int y, uint8_t lum) {
        uint8_t* p = &img[((size_t)y * w + x) * 4];
        p[0] = p[1] = p[2] = lum;
        p[3] = 255;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 220);
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x)
            fill(x, y, 60);
    for (int y = 12; y < 20; ++y)
        for (int x = 12; x < 20; ++x)
            fill(x, y, 90); // enclosed and sharp-rimmed, but 90 < 220/2 —
                            // too dark for shadowed backdrop, no seed

    const SilhouetteMask m = BinarizeImage(img.data(), w, h);
    CHECK(MaskArea(m) == 16 * 16); // solid object, recess kept
    CHECK(m.pixels[15 * w + 15] != 0);
}

// The chroma gate on recovery: a recess whose LUMINANCE reads as shadowed
// backdrop but whose color is tinted is object material — backdrop seen
// through a hole keeps the ground's neutrality. (The axe's brass vs the white
// sweep.)
static void BinarizeTintedRecessKept()
{
    const int w = 32, h = 32;
    std::vector<uint8_t> img((size_t)w * h * 4);
    auto fill = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = &img[((size_t)y * w + x) * 4];
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 255;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 220, 220, 220);
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x)
            fill(x, y, 60, 60, 60);
    for (int y = 12; y < 20; ++y)
        for (int x = 12; x < 20; ++x)
            fill(x, y, 180, 140, 100); // lum 147 sits in the shadow window,
                                       // but spread 80 is nothing like the ground

    const SilhouetteMask m = BinarizeImage(img.data(), w, h);
    CHECK(MaskArea(m) == 16 * 16); // solid object, tinted recess kept
    CHECK(m.pixels[15 * w + 15] != 0);
}

// On a COLORED ground the chroma guard has nothing to say, so hole recovery
// must not run at all — with the caps wide open it would punch this tinted
// recess on luminance alone (its lum 120 sits mid shadow-window of the warm
// ground's 226). Colored grounds keep pre-#134 behavior exactly.
static void BinarizeColoredGroundNoRecovery()
{
    const int w = 32, h = 32;
    std::vector<uint8_t> img((size_t)w * h * 4);
    auto fill = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = &img[((size_t)y * w + x) * 4];
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 255;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 240, 225, 200); // warm studio paper, spread 40
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x)
            fill(x, y, 60, 60, 60);
    for (int y = 12; y < 20; ++y)
        for (int x = 12; x < 20; ++x)
            fill(x, y, 160, 110, 70); // enclosed tinted recess, sharp-rimmed

    const SilhouetteMask m = BinarizeImage(img.data(), w, h);
    CHECK(MaskArea(m) == 16 * 16); // solid object, recess kept
    CHECK(m.pixels[15 * w + 15] != 0);
}

// The chroma gate on the flood itself (#134's blade-leak case): a warm-tinted
// bright object whose rim ramps into a white ground at under kEdgeStep per
// pixel. The gradient gate alone would roll straight through; the border's
// chroma model says the object is not ground no matter how bright.
static void BinarizeTintedObjectOnWhiteKept()
{
    const int w = 32, h = 32;
    std::vector<uint8_t> img((size_t)w * h * 4);
    auto fill = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        uint8_t* p = &img[((size_t)y * w + x) * 4];
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 255;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 250, 250, 250); // pure white ground
    for (int y = 10; y < 22; ++y)
        for (int x = 10; x < 22; ++x)
            fill(x, y, 252, 244, 236); // lum 245: one gradient step from the
                                       // ground, but visibly warm (spread 16)
    for (int y = 13; y < 19; ++y)
        for (int x = 13; x < 19; ++x)
            fill(x, y, 60, 60, 60); // dark core: with the gate broken the
                                    // flood eats the warm rim and SUCCEEDS
                                    // with just this core (36 px), instead of
                                    // degenerating into an Otsu rescue that
                                    // would pass the assertions anyway

    const SilhouetteMask m = BinarizeImage(img.data(), w, h);
    // The object survives, plus a thin blur-halo of ground pixels whose 9x9
    // chroma window overlaps it — the price of denoising the gate. Bounded,
    // not exact: the halo shape depends on integer blur rounding.
    const int area = MaskArea(m);
    CHECK(area >= 12 * 12);
    CHECK(area <= 20 * 20);
    CHECK(m.pixels[16 * w + 16] != 0); // object center is figure
    CHECK(m.pixels[11 * w + 11] != 0); // warm rim is figure
    CHECK(m.pixels[4 * w + 4] == 0);   // ground is ground
}

// Specular highlight vs hole: sheen fades SMOOTHLY into the object, so the
// recovery region leaks down the ramp across the whole figure and disqualifies
// itself (open + over the area cap) — where a true hole's sharp rim would have
// contained it. The issue's brushed-steel-sheen case.
static void BinarizeHighlightNotPunched()
{
    const int w = 96, h = 96;
    std::vector<uint8_t> img((size_t)w * h * 4);
    auto fill = [&](int x, int y, uint8_t lum) {
        uint8_t* p = &img[((size_t)y * w + x) * 4];
        p[0] = p[1] = p[2] = lum;
        p[3] = 255;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 220); // ground
    for (int y = 8; y < 88; ++y)
        for (int x = 8; x < 88; ++x) {
            // Dark object with a highlight core near ground tone; the ramp
            // slope (5/px) never exceeds the flood's gradient gate.
            const int c = std::max(std::abs(x - 48), std::abs(y - 48));
            fill(x, y, (uint8_t)std::max(60, 215 - 5 * c));
        }

    const SilhouetteMask m = BinarizeImage(img.data(), w, h);
    CHECK(MaskArea(m) == 80 * 80); // nothing punched
    CHECK(m.pixels[48 * w + 48] != 0);
}

// The topology guard: a shadow-toned pocket that ANCHORS real figure (dark
// decoration enclosed inside it) must not punch — that would orphan the
// decoration onto background. The pocket passes the enclosure, area, and
// shadow-window tests (it is only 8% of the figure and sits mid-window), so
// containment is the ONLY thing keeping it with the object.
static void BinarizePocketAnchoringIslandKept()
{
    const int w = 48, h = 48;
    std::vector<uint8_t> img((size_t)w * h * 4);
    auto fill = [&](int x, int y, uint8_t lum) {
        uint8_t* p = &img[((size_t)y * w + x) * 4];
        p[0] = p[1] = p[2] = lum;
        p[3] = 255;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            fill(x, y, 250); // white ground
    for (int y = 4; y < 44; ++y)
        for (int x = 4; x < 44; ++x)
            fill(x, y, 120); // 40x40 object
    for (int y = 12; y < 24; ++y)
        for (int x = 12; x < 24; ++x)
            fill(x, y, 160); // shadow-window pocket, sharp-rimmed, enclosed
    for (int y = 16; y < 20; ++y)
        for (int x = 16; x < 20; ++x)
            fill(x, y, 40); // decoration island inside the pocket

    const SilhouetteMask m = BinarizeImage(img.data(), w, h);
    CHECK(MaskArea(m) == 40 * 40);     // whole object, pocket kept
    CHECK(m.pixels[13 * w + 13] != 0); // pocket is figure
    CHECK(m.pixels[17 * w + 17] != 0); // decoration too
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

// --- outline extraction (#135) ------------------------------------------------

// Filled rect: one positive-area outer contour, collinear-merged to 4 points,
// parent -1, and an EXACT round-trip (the rasterizer is the trace's inverse).
static void TraceRectExact()
{
    const SilhouetteMask m = SolidRect(20, 20, 3, 4, 8, 6); // 8x6 rect
    const std::vector<SilhouetteContour> cs = TraceContours(m);
    CHECK(cs.size() == 1);
    if (cs.size() == 1) {
        CHECK(cs[0].points.size() == 4);
        CHECK(cs[0].area == 8.0 * 6.0); // positive == w*h
        CHECK(!cs[0].hole);
        CHECK(cs[0].parent == -1);
    }
    CHECK(RasterizePolygons(cs, m.width, m.height).pixels == m.pixels); // memcmp
}

// Rect with a rect hole: two contours, the hole negative-area with parent =
// outer; even-odd re-rasterization preserves the hole exactly.
static void TraceDonut()
{
    SilhouetteMask m = SolidRect(30, 30, 5, 5, 18, 18);
    for (int y = 10; y < 18; ++y)
        for (int x = 10; x < 18; ++x)
            m.pixels[(size_t)y * 30 + x] = 0; // 8x8 hole
    const std::vector<SilhouetteContour> cs = TraceContours(m);
    CHECK(cs.size() == 2);
    int outer = -1, hole = -1;
    for (int i = 0; i < (int)cs.size(); ++i)
        (cs[i].hole ? hole : outer) = i;
    CHECK(outer >= 0 && hole >= 0);
    if (outer >= 0 && hole >= 0) {
        CHECK(cs[outer].area > 0.0);
        CHECK(cs[hole].area < 0.0);
        CHECK(cs[hole].parent == outer);
        CHECK(cs[outer].parent == -1);
    }
    CHECK(RasterizePolygons(cs, m.width, m.height).pixels == m.pixels);
}

// 2x2 diagonal checkerboard: the saddle rule crosses over so the two diagonal
// squares read as ONE 8-connected loop (pinned here). Round-trip is exact
// whichever way the saddle resolves, so BOTH assertions matter.
static void TraceSaddleCheckerboard()
{
    SilhouetteMask m = MakeMask(2, 2);
    m.pixels = {255, 0, 0, 255}; // covered (0,0) and (1,1)
    const std::vector<SilhouetteContour> cs = TraceContours(m);
    CHECK(cs.size() == 1); // 8-connected: one loop, not two
    if (cs.size() == 1)
        CHECK(cs[0].area == 2.0); // two unit squares
    CHECK(RasterizePolygons(cs, 2, 2).pixels == m.pixels);
}

// Two disjoint rects: two top-level outers, exact round-trip.
static void TraceMultiComponent()
{
    SilhouetteMask m = MakeMask(40, 20);
    auto set = [&](int x0, int x1, int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                m.pixels[(size_t)y * 40 + x] = 255;
    };
    set(3, 10, 3, 10);
    set(25, 35, 5, 15);
    const std::vector<SilhouetteContour> cs = TraceContours(m);
    CHECK(cs.size() == 2);
    int tops = 0;
    for (const SilhouetteContour& c : cs)
        if (c.parent == -1 && !c.hole)
            ++tops;
    CHECK(tops == 2);
    CHECK(RasterizePolygons(cs, m.width, m.height).pixels == m.pixels);
}

// A filled disc for the round-trip / budget suites (center-sampled).
static SilhouetteMask FilledDisc(int size, double cx, double cy, double r)
{
    SilhouetteMask m = MakeMask(size, size);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            if (dx * dx + dy * dy <= r * r)
                m.pixels[(size_t)y * size + x] = 255;
        }
    return m;
}

// The issue's shapes (notch / crescent / donut): full-res round-trip is EXACT;
// simplified to budget 64 / tol 1.0 it re-rasterizes to IoU >= 0.98.
static void RoundTripNotchCrescentDonut()
{
    const int W = 200;
    auto roundTripEqual = [&](const SilhouetteMask& src) {
        return RasterizePolygons(TraceContours(src), src.width, src.height).pixels ==
               src.pixels;
    };
    auto simplifiedIoU = [&](const SilhouetteMask& src) {
        std::vector<SilhouetteContour> cs = TraceContours(src);
        for (SilhouetteContour& c : cs)
            c.points = SimplifyContour(c.points, 64, 1.0f);
        return CompareMasks(RasterizePolygons(cs, src.width, src.height), src).iou;
    };

    // Rect with a notch bitten out of the top edge (open concavity, no hole).
    SilhouetteMask notch = SolidRect(W, W, 40, 40, 120, 100);
    for (int y = 40; y < 80; ++y)
        for (int x = 80; x < 120; ++x)
            notch.pixels[(size_t)y * W + x] = 0;
    CHECK(roundTripEqual(notch));
    CHECK(simplifiedIoU(notch) >= 0.98f);

    // Crescent: a disc minus an offset disc of equal radius (a fat crescent
    // moon — blunt enough cusps to survive tol-1.0 simplification cleanly).
    SilhouetteMask crescent = FilledDisc(W, 85, 100, 72);
    for (int y = 0; y < W; ++y)
        for (int x = 0; x < W; ++x) {
            const double dx = x + 0.5 - 128, dy = y + 0.5 - 100;
            if (dx * dx + dy * dy <= 66.0 * 66.0)
                crescent.pixels[(size_t)y * W + x] = 0;
        }
    CHECK(MaskArea(crescent) > 0);
    CHECK(roundTripEqual(crescent));
    CHECK(simplifiedIoU(crescent) >= 0.98f);

    // Donut: a disc with a concentric disc hole.
    SilhouetteMask donut = FilledDisc(W, 100, 100, 70);
    for (int y = 0; y < W; ++y)
        for (int x = 0; x < W; ++x) {
            const double dx = x + 0.5 - 100, dy = y + 0.5 - 100;
            if (dx * dx + dy * dy <= 35.0 * 35.0)
                donut.pixels[(size_t)y * W + x] = 0;
        }
    CHECK(roundTripEqual(donut));
    CHECK(simplifiedIoU(donut) >= 0.98f);
}

// Budget is a hard cap and more budget never scores worse (monotone, loose).
static void SimplifyBudgetRespected()
{
    const SilhouetteMask disc = FilledDisc(120, 60, 60, 50);
    const std::vector<SilhouetteContour> cs = TraceContours(disc);
    CHECK(cs.size() == 1);
    if (cs.empty())
        return;
    float iou8 = 0.0f, iou64 = 0.0f;
    for (int budget : {8, 16, 64}) {
        std::vector<SilhouetteContour> s = cs;
        s[0].points = SimplifyContour(cs[0].points, budget, 0.0f); // tol 0: budget binds
        CHECK((int)s[0].points.size() <= budget);
        const float iou = CompareMasks(RasterizePolygons(s, 120, 120), disc).iou;
        if (budget == 8)
            iou8 = iou;
        else if (budget == 64)
            iou64 = iou;
    }
    CHECK(iou64 >= iou8);
}

// Collinear runs cost nothing (a traced rect stays 4 points under a big budget)
// and real corners are never dropped (a hand-built octagon keeps all 8).
static void SimplifyCollinear()
{
    const SilhouetteMask r = SolidRect(30, 30, 5, 5, 15, 10);
    const std::vector<SilhouetteContour> cs = TraceContours(r);
    CHECK(cs.size() == 1);
    if (!cs.empty())
        CHECK(SimplifyContour(cs[0].points, 64, 1.0f).size() == 4);
    const std::vector<vec2> oct = {{10, 0}, {20, 0}, {30, 10}, {30, 20},
                                   {20, 30}, {10, 30}, {0, 20}, {0, 10}};
    CHECK(SimplifyContour(oct, 64, 1.0f).size() == 8);
}

// Exact landmarks on a T-shape whose widest row (top bar) and tallest column
// (stem) are deliberately different features.
static void LandmarksRect()
{
    SilhouetteMask m = MakeMask(16, 16);
    auto set = [&](int x0, int x1, int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                m.pixels[(size_t)y * 16 + x] = 255;
    };
    set(2, 14, 2, 5);  // wide top bar (12 x 3)
    set(6, 10, 5, 12); // narrow stem (4 x 7)
    const SilhouetteLandmarks lm = MaskLandmarks(m);
    CHECK(lm.minX == 2 && lm.maxX == 13 && lm.minY == 2 && lm.maxY == 11);
    CHECK(lm.bboxWidth == 12 && lm.bboxHeight == 10);
    CHECK(ApproxEq(lm.aspect, 1.2f));
    CHECK(lm.area == 64);
    CHECK(ApproxEq(lm.centroid.x, 8.0f) && ApproxEq(lm.centroid.y, 5.6875f));
    CHECK(lm.widestRowSpan == 12 && lm.widestRowY == 2 && lm.widestRowLeft == 2 &&
          lm.widestRowRight == 13);
    CHECK(lm.tallestColSpan == 10 && lm.tallestColX == 6 && lm.tallestColTop == 2 &&
          lm.tallestColBottom == 11);
    CHECK(ApproxEq(lm.top.x, 8.0f) && ApproxEq(lm.top.y, 2.5f));
    CHECK(ApproxEq(lm.bottom.x, 8.0f) && ApproxEq(lm.bottom.y, 11.5f));
    CHECK(ApproxEq(lm.left.x, 2.5f) && ApproxEq(lm.left.y, 3.5f));
    CHECK(ApproxEq(lm.right.x, 13.5f) && ApproxEq(lm.right.y, 3.5f));
    CHECK(ApproxEq(lm.fillFactor, 64.0f / (12.0f * 10.0f)));
    CHECK(ApproxEq(lm.areaFractionImage, 64.0f / 256.0f));
}

// Exact row-span triplets on a T-shape sized so rows=3 samples integer rows.
static void RowSpansTable()
{
    SilhouetteMask m = MakeMask(16, 16);
    auto set = [&](int x0, int x1, int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                m.pixels[(size_t)y * 16 + x] = 255;
    };
    set(2, 14, 1, 4);  // top bar rows 1..3
    set(6, 10, 4, 10); // stem rows 4..9; bbox height 9 -> rows 1, 5, 9 sampled
    const std::vector<SilhouetteRowSpan> s = MaskRowSpans(m, 3);
    CHECK(s.size() == 3);
    if (s.size() == 3) {
        CHECK(s[0].y == 1 && s[0].left == 2 && s[0].right == 13 && s[0].covered == 12);
        CHECK(s[1].y == 5 && s[1].left == 6 && s[1].right == 9 && s[1].covered == 4);
        CHECK(s[2].y == 9 && s[2].left == 6 && s[2].right == 9 && s[2].covered == 4);
    }
}

// Fold a symmetric vase's outline back to r(y) and confirm every folded radius
// tracks the generating profile within 1.5 px, both ends present.
static void FoldVaseProfile()
{
    const int W = 100;
    const float cx = 50.0f;
    struct Ctrl {
        float y, r;
    };
    const std::vector<Ctrl> prof = {{10, 8}, {30, 6}, {60, 20}, {90, 12}};
    auto radiusAt = [&](float y) -> float {
        if (y <= prof.front().y)
            return prof.front().r;
        if (y >= prof.back().y)
            return prof.back().r;
        for (size_t i = 1; i < prof.size(); ++i)
            if (y <= prof[i].y) {
                const float t = (y - prof[i - 1].y) / (prof[i].y - prof[i - 1].y);
                return prof[i - 1].r + t * (prof[i].r - prof[i - 1].r);
            }
        return prof.back().r;
    };
    SilhouetteMask m = MakeMask(W, W);
    for (int y = (int)prof.front().y; y <= (int)prof.back().y; ++y) {
        const float r = radiusAt((float)y + 0.5f);
        for (int x = 0; x < W; ++x)
            if (std::fabs((float)x + 0.5f - cx) <= r)
                m.pixels[(size_t)y * W + x] = 255;
    }
    const std::vector<SilhouetteContour> cs = TraceContours(m);
    CHECK(cs.size() == 1);
    if (cs.empty())
        return;
    const std::vector<vec2> fold =
        FoldOutline(SimplifyContour(cs[0].points, 128, 1.0f), cx);
    CHECK(fold.size() >= 2);
    float maxErr = 0.0f;
    for (const vec2& p : fold)
        maxErr = std::max(maxErr, std::fabs(p.x - radiusAt(p.y)));
    CHECK(maxErr <= 1.5f);
    CHECK(fold.front().y > fold.back().y);         // bottom (max y) -> top (min y)
    CHECK(ApproxEq(fold.front().y, 90.0f, 1.5f));  // bottom end present
    CHECK(ApproxEq(fold.back().y, 10.0f, 1.5f));   // top end present
}

// The two-valued gray fast path: a silhouette PNG re-fed keeps its enclosed
// hole (the dark-ground flood would weld it shut — this test fails if the fast
// path is removed), and the polarity flip yields the identical mask.
static void BinaryImageFastPath()
{
    const int W = 32;
    SilhouetteMask donut = SolidRect(W, W, 8, 8, 16, 16);
    for (int y = 13; y < 19; ++y)
        for (int x = 13; x < 19; ++x)
            donut.pixels[(size_t)y * W + x] = 0; // enclosed hole
    std::vector<uint8_t> img((size_t)W * W * 4, 0);
    auto paint = [&](bool figureWhite) {
        for (int i = 0; i < W * W; ++i) {
            const bool fig = donut.pixels[(size_t)i] != 0;
            const uint8_t v = (fig == figureWhite) ? 255 : 0;
            img[(size_t)i * 4 + 0] = img[(size_t)i * 4 + 1] = img[(size_t)i * 4 + 2] = v;
            img[(size_t)i * 4 + 3] = 255; // opaque, like a compare_silhouette PNG
        }
    };
    paint(true); // white figure on black ground
    CHECK(BinarizeImage(img.data(), W, W).pixels == donut.pixels);
    paint(false); // black figure on white ground -> same mask
    CHECK(BinarizeImage(img.data(), W, W).pixels == donut.pixels);
}

// A 3-valued gray image (250 / 60 / 246) is NOT the fast path — the enclosed
// same-tone region (246, deliberately a third value, not 250) goes through the
// flood and stays FIGURE, because 246 is far above the shadow ceiling. This
// guards the scope trap: a 250/60/250 image would be two-valued and skip Otsu.
static void BinaryFastPathNotTakenOnPhotoLikeInput()
{
    const int W = 32;
    std::vector<uint8_t> img((size_t)W * W * 4, 255);
    auto fill = [&](int x, int y, uint8_t lum) {
        uint8_t* p = &img[((size_t)y * W + x) * 4];
        p[0] = p[1] = p[2] = lum;
        p[3] = 255;
    };
    for (int y = 0; y < W; ++y)
        for (int x = 0; x < W; ++x)
            fill(x, y, 250); // ground
    for (int y = 8; y < 24; ++y)
        for (int x = 8; x < 24; ++x)
            fill(x, y, 60); // object ring (dark outline seals the flood out)
    for (int y = 13; y < 19; ++y)
        for (int x = 13; x < 19; ++x)
            fill(x, y, 246); // enclosed same-tone region, a THIRD value
    const SilhouetteMask m = BinarizeImage(img.data(), W, W);
    CHECK(m.pixels[(size_t)15 * W + 15] != 0); // enclosed region stays figure
    CHECK(m.pixels[(size_t)9 * W + 9] != 0);   // ring is figure
    CHECK(m.pixels[(size_t)2 * W + 2] == 0);   // ground is ground
    CHECK(MaskArea(m) == 16 * 16);             // whole object solid, no punch
}

// The area floor (#142 review): a figure peppered with pinholes (grainy Otsu
// output) traces to 257 loops by default — the exactness contract — while
// minArea 4.0 drops every 1-px hole before the quadratic parent pass.
static void TracePinholeFloor()
{
    SilhouetteMask m = SolidRect(64, 64, 0, 0, 64, 64);
    for (int gy = 0; gy < 16; ++gy)
        for (int gx = 0; gx < 16; ++gx)
            m.pixels[(size_t)(4 * gy + 2) * 64 + (4 * gx + 2)] = 0; // 256 pinholes
    CHECK(TraceContours(m).size() == 257); // default: exact, everything kept
    const std::vector<SilhouetteContour> floored = TraceContours(m, 4.0);
    CHECK(floored.size() == 1);
    if (!floored.empty()) {
        CHECK(!floored[0].hole);
        CHECK(floored[0].parent == -1);
    }
}

// A 1-px hole's 4-point loop must survive simplification as a real polygon
// (#142 review): its corner deviation (~0.707 px) sits under tolerance 1.0,
// and the early-out used to fire with only the two anchors kept — a 2-point
// "polygon" the rasterizer silently skips, welding the hole shut.
static void SimplifyTinyHoleStaysPolygon()
{
    SilhouetteMask m = SolidRect(12, 12, 2, 2, 8, 8);
    m.pixels[(size_t)6 * 12 + 6] = 0; // 1-px hole
    std::vector<SilhouetteContour> cs = TraceContours(m);
    CHECK(cs.size() == 2);
    int hole = -1;
    for (int i = 0; i < (int)cs.size(); ++i)
        if (cs[i].hole)
            hole = i;
    CHECK(hole >= 0);
    if (hole < 0)
        return;
    CHECK(cs[hole].points.size() == 4);
    for (SilhouetteContour& c : cs)
        c.points = SimplifyContour(c.points, 32, 1.0f);
    CHECK(cs[hole].points.size() >= 3);
    // IoU on a 1-px hole is brittle; the pin is the hole pixel itself: a
    // 3-point simplification of the unit square still leaves its center
    // outside the covered set under even-odd fill.
    const SilhouetteMask rt = RasterizePolygons(cs, 12, 12);
    CHECK(rt.pixels[(size_t)6 * 12 + 6] == 0); // hole pixel stays uncovered
    CHECK(rt.pixels[(size_t)6 * 12 + 5] != 0); // its neighbor stays figure
}

// Parent CHAIN through three levels (#142 review): a blob with a hole with an
// island inside the hole. The island is an OUTER contour whose parent is the
// HOLE — emission must not lose it, and the trace stays round-trip exact.
static void TraceIslandParentChain()
{
    SilhouetteMask m = SolidRect(30, 30, 2, 2, 26, 26);
    for (int y = 6; y < 24; ++y)
        for (int x = 6; x < 24; ++x)
            m.pixels[(size_t)y * 30 + x] = 0; // hole
    for (int y = 10; y < 20; ++y)
        for (int x = 10; x < 20; ++x)
            m.pixels[(size_t)y * 30 + x] = 255; // island inside the hole
    const std::vector<SilhouetteContour> cs = TraceContours(m);
    CHECK(cs.size() == 3);
    int outer = -1, hole = -1, island = -1;
    for (int i = 0; i < (int)cs.size(); ++i) {
        if (cs[i].hole)
            hole = i;
        else if (cs[i].parent == -1)
            outer = i;
        else
            island = i;
    }
    CHECK(outer >= 0 && hole >= 0 && island >= 0);
    if (outer < 0 || hole < 0 || island < 0)
        return;
    CHECK(cs[hole].parent == outer);
    CHECK(cs[island].parent == hole);
    CHECK(!cs[island].hole && cs[island].area > 0.0);
    CHECK(RasterizePolygons(cs, 30, 30).pixels == m.pixels);
}

// --- structured diff (#136) -----------------------------------------------------

// ORs a rect into an existing mask (SolidRect makes a fresh one).
static void FillRect(SilhouetteMask& m, int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            m.pixels[(size_t)y * m.width + x] = 255;
}

// Two known B-only blobs: exact areas, fractions, centroids, bboxes, ordering;
// swapping the operands flips the class and nothing else.
static void DiffRegionsKnownBlobs()
{
    const SilhouetteMask a = SolidRect(32, 32, 2, 2, 10, 10);
    SilhouetteMask b = SolidRect(32, 32, 2, 2, 10, 10);
    FillRect(b, 20, 10, 6, 4); // 24 px
    FillRect(b, 24, 24, 3, 3); // 9 px; union(A,B) = 100 + 24 + 9 = 133

    const DiffRegionList list = DiffRegions(a, b);
    CHECK(list.totalRegions == 2);
    CHECK(list.droppedRegions == 0);
    CHECK(list.droppedArea == 0);
    CHECK(list.regions.size() == 2);
    if (list.regions.size() == 2) {
        const DiffRegion& r0 = list.regions[0]; // larger first
        CHECK(!r0.excess); // B-only = missing
        CHECK(r0.area == 24);
        CHECK(r0.areaFraction == 24.0f / 133.0f);
        CHECK(r0.centroid.x == 23.0f && r0.centroid.y == 12.0f);
        CHECK(r0.minX == 20 && r0.minY == 10 && r0.maxX == 25 && r0.maxY == 13);
        const DiffRegion& r1 = list.regions[1];
        CHECK(!r1.excess);
        CHECK(r1.area == 9);
        CHECK(r1.areaFraction == 9.0f / 133.0f);
        CHECK(r1.centroid.x == 25.5f && r1.centroid.y == 25.5f);
        CHECK(r1.minX == 24 && r1.minY == 24 && r1.maxX == 26 && r1.maxY == 26);
    }

    // Swapped operands: identical geometry, typed excess (A-only) now.
    const DiffRegionList sw = DiffRegions(b, a);
    CHECK(sw.totalRegions == 2);
    CHECK(sw.regions.size() == 2);
    if (sw.regions.size() == 2) {
        CHECK(sw.regions[0].excess && sw.regions[1].excess);
        CHECK(sw.regions[0].area == 24 && sw.regions[1].area == 9);
        CHECK(sw.regions[0].centroid.x == 23.0f && sw.regions[0].centroid.y == 12.0f);
        CHECK(sw.regions[1].centroid.x == 25.5f && sw.regions[1].centroid.y == 25.5f);
        CHECK(sw.regions[0].minX == 20 && sw.regions[0].minY == 10 &&
              sw.regions[0].maxX == 25 && sw.regions[0].maxY == 13);
        CHECK(sw.regions[1].minX == 24 && sw.regions[1].minY == 24 &&
              sw.regions[1].maxX == 26 && sw.regions[1].maxY == 26);
    }
}

// An A-only strip ADJACENT to a B-only blob: one region per class (touching
// pixels of different classes never join), and kept + dropped areas equal
// CompareMasks' onlyA + onlyB — the accounting invariant.
static void DiffRegionsMixedClasses()
{
    SilhouetteMask a = SolidRect(32, 32, 2, 2, 10, 10);
    SilhouetteMask b = SolidRect(32, 32, 2, 2, 10, 10);
    FillRect(a, 2, 20, 8, 2);  // A-only strip, 16 px, x in [2,10)
    FillRect(b, 10, 20, 5, 5); // B-only blob, 25 px, shares the x=9|10 border

    const SilhouetteDiff d = CompareMasks(a, b);
    CHECK(d.onlyA == 16 && d.onlyB == 25);

    const DiffRegionList list = DiffRegions(a, b);
    CHECK(list.totalRegions == 2);
    CHECK(list.regions.size() == 2);
    int excess = 0, missing = 0, keptArea = 0;
    for (const DiffRegion& r : list.regions) {
        ++(r.excess ? excess : missing);
        keptArea += r.area;
    }
    CHECK(excess == 1 && missing == 1);
    CHECK(keptArea + list.droppedArea == d.onlyA + d.onlyB);
}

// Two 2x2 blobs touching only at a corner are TWO regions under 4-connectivity.
static void DiffRegionsFourConnectivity()
{
    const SilhouetteMask a = MakeMask(16, 16);
    SilhouetteMask b = MakeMask(16, 16);
    FillRect(b, 4, 4, 2, 2);
    FillRect(b, 6, 6, 2, 2); // diagonal neighbor of the first at (5,5)/(6,6)

    const DiffRegionList list = DiffRegions(a, b);
    CHECK(list.totalRegions == 2); // NOT one 8-connected blob
    CHECK(list.regions.size() == 2);
    if (list.regions.size() == 2) {
        // Equal areas: minY breaks the tie, top-left blob first.
        CHECK(list.regions[0].area == 4 && list.regions[1].area == 4);
        CHECK(list.regions[0].minX == 4 && list.regions[0].minY == 4 &&
              list.regions[0].maxX == 5 && list.regions[0].maxY == 5);
        CHECK(list.regions[1].minX == 6 && list.regions[1].minY == 6 &&
              list.regions[1].maxX == 7 && list.regions[1].maxY == 7);
    }
}

// A 1-px mismatch on a big-union pair sits under the 0.2% default floor: it is
// dropped but stays on the books in the counters.
static void DiffRegionsSpeckSuppression()
{
    const SilhouetteMask a = SolidRect(64, 64, 2, 2, 30, 30); // 900 px shared
    SilhouetteMask b = SolidRect(64, 64, 2, 2, 30, 30);
    FillRect(b, 40, 40, 10, 10);           // 100 px real region
    b.pixels[(size_t)2 * 64 + 60] = 255;   // 1 px speck: 1/1001 < 0.002

    const DiffRegionList list = DiffRegions(a, b);
    CHECK(list.totalRegions == 2);
    CHECK(list.regions.size() == 1);
    if (!list.regions.empty()) {
        CHECK(list.regions[0].area == 100);
        CHECK(!list.regions[0].excess);
    }
    CHECK(list.droppedRegions == 1);
    CHECK(list.droppedArea == 1);
}

// The cap keeps the maxRegions largest (still sorted) and summarizes the rest.
static void DiffRegionsCapAndSummarize()
{
    const SilhouetteMask a = MakeMask(64, 64);
    SilhouetteMask b = MakeMask(64, 64);
    // Six disjoint blobs, strictly descending areas: 49 36 25 16 9 4.
    FillRect(b, 1, 1, 7, 7);
    FillRect(b, 10, 1, 6, 6);
    FillRect(b, 20, 1, 5, 5);
    FillRect(b, 30, 1, 4, 4);
    FillRect(b, 40, 1, 3, 3);
    FillRect(b, 50, 1, 2, 2);

    const DiffRegionList list = DiffRegions(a, b, 4, 0.0f);
    CHECK(list.totalRegions == 6);
    CHECK(list.regions.size() == 4);
    if (list.regions.size() == 4) {
        CHECK(list.regions[0].area == 49);
        CHECK(list.regions[1].area == 36);
        CHECK(list.regions[2].area == 25);
        CHECK(list.regions[3].area == 16);
    }
    CHECK(list.droppedRegions == 2);
    CHECK(list.droppedArea == 9 + 4);
}

// Mismatched or degenerate sizes: empty result, mirroring CompareMasks.
static void DiffRegionsSizeMismatchEmpty()
{
    const DiffRegionList list = DiffRegions(MakeMask(16, 16), MakeMask(32, 32));
    CHECK(list.regions.empty());
    CHECK(list.totalRegions == 0);
    CHECK(list.droppedRegions == 0 && list.droppedArea == 0);
    CHECK(DiffRegions(MakeMask(0, 0), MakeMask(0, 0)).totalRegions == 0);
}

// Bilateral symmetry scoring about the covered bbox's center line.
static void MirrorSymmetryScores()
{
    // Cross pentomino centered at (8,8): pixel-perfect about its bbox center.
    SilhouetteMask m = MakeMask(16, 16);
    auto set = [](SilhouetteMask& mask, int x, int y) {
        mask.pixels[(size_t)y * mask.width + x] = 255;
    };
    set(m, 8, 7);
    set(m, 7, 8);
    set(m, 8, 8);
    set(m, 9, 8);
    set(m, 8, 9);
    CHECK(MaskMirrorSymmetryX(m) == 1.0f);

    // One extra off-axis pixel (bbox unchanged): |M|=6, mirror hits 5 of them
    // -> IoU = 5 / (2*6 - 5) = 5/7 by hand.
    set(m, 9, 9);
    CHECK(MaskMirrorSymmetryX(m) == 5.0f / 7.0f);

    // No shape is not a symmetric shape.
    CHECK(MaskMirrorSymmetryX(MakeMask(8, 8)) == 0.0f);

    // Off-center placement still scores 1.0: the mirror axis is the bbox
    // center, not the frame center.
    SilhouetteMask off = MakeMask(32, 32);
    set(off, 21, 3);
    set(off, 20, 4);
    set(off, 21, 4);
    set(off, 22, 4);
    set(off, 21, 5);
    CHECK(MaskMirrorSymmetryX(off) == 1.0f);
}

// NormalizeMask's transform-out overload reports the exact crop/scale/pad, and
// NormalizedToSourcePx inverts it back to source pixels.
static void NormalizeTransformRoundTrip()
{
    const SilhouetteMask src = SolidRect(64, 64, 40, 10, 8, 6); // off-center 8x6
    NormalizeTransform xform;
    const SilhouetteMask norm = NormalizeMask(src, 32, xform);
    CHECK(xform.valid);
    CHECK(xform.srcMinX == 40 && xform.srcMinY == 10);
    CHECK(xform.srcW == 8 && xform.srcH == 6);
    CHECK(xform.scaledW == 32 && xform.scaledH == 24); // 6 * 32/8, x fills
    CHECK(xform.padX == 0 && xform.padY == 4);         // (32 - 24) / 2

    // Normalized content center -> source rect center (44, 13).
    const vec2 srcC = NormalizedToSourcePx(
        xform, vec2((float)xform.padX + 0.5f * (float)xform.scaledW,
                    (float)xform.padY + 0.5f * (float)xform.scaledH));
    CHECK(std::fabs(srcC.x - 44.0f) <= 0.75f);
    CHECK(std::fabs(srcC.y - 13.0f) <= 0.75f);

    // The two-arg overload delegates: byte-identical output.
    CHECK(NormalizeMask(src, 32).pixels == norm.pixels);

    // No coverage -> invalid transform (stale caller state overwritten) and
    // NormalizedToSourcePx passes coordinates through unchanged.
    NormalizeTransform none;
    none.valid = true;
    (void)NormalizeMask(MakeMask(16, 16), 32, none);
    CHECK(!none.valid);
    const vec2 same = NormalizedToSourcePx(none, vec2(5.0f, 7.0f));
    CHECK(same.x == 5.0f && same.y == 7.0f);
}

// Viewport inversion on the front view: mask center at the bounds-center depth
// comes back as the bounds center, the frame's left edge as center minus the
// ortho half-extent — both derived from SilhouetteViewProj's own constants.
static void MaskPxToWorldFrontView()
{
    AABB box;
    box.Expand({-1.0f, -2.0f, -0.5f});
    box.Expand({3.0f, 2.0f, 0.5f});
    const std::optional<mat4> vp = SilhouetteViewProj("front", box);
    CHECK(vp.has_value());
    if (!vp)
        return;
    const vec3 c = (box.min + box.max) * 0.5f; // (1, 0, 0)
    const vec4 clip = *vp * vec4(c, 1.0f);
    const float ndcZ = clip.z / clip.w;

    const vec3 center = MaskPxToWorld(*vp, 256, 256, vec2(128.0f, 128.0f), ndcZ);
    CHECK(ApproxEq(center.x, c.x, 1e-3f));
    CHECK(ApproxEq(center.y, c.y, 1e-3f));

    // Same margin constant as SilhouetteViewProj: half = larger in-plane
    // half-extent * 1.02 (= 2.04 here) — no magic floats.
    const vec3 he = glm::max((box.max - box.min) * 0.5f, vec3(1e-4f));
    const float half = std::max(he.x, he.y) * 1.02f;
    const vec3 leftEdge = MaskPxToWorld(*vp, 256, 256, vec2(0.0f, 128.0f), ndcZ);
    CHECK(ApproxEq(leftEdge.x, c.x - half, 1e-3f));
    CHECK(ApproxEq(leftEdge.y, c.y, 1e-3f));
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
    BinarizeDonutHoleRecovered();
    BinarizeDarkRecessKept();
    BinarizeTintedRecessKept();
    BinarizeColoredGroundNoRecovery();
    BinarizeTintedObjectOnWhiteKept();
    BinarizeHighlightNotPunched();
    BinarizePocketAnchoringIslandKept();
    NormalizeInvariances();
    CompareAndDiffValues();
    LatheCupAcceptance();
    TraceRectExact();
    TraceDonut();
    TraceSaddleCheckerboard();
    TraceMultiComponent();
    RoundTripNotchCrescentDonut();
    SimplifyBudgetRespected();
    SimplifyCollinear();
    LandmarksRect();
    RowSpansTable();
    FoldVaseProfile();
    BinaryImageFastPath();
    BinaryFastPathNotTakenOnPhotoLikeInput();
    TracePinholeFloor();
    SimplifyTinyHoleStaysPolygon();
    TraceIslandParentChain();
    DiffRegionsKnownBlobs();
    DiffRegionsMixedClasses();
    DiffRegionsFourConnectivity();
    DiffRegionsSpeckSuppression();
    DiffRegionsCapAndSummarize();
    DiffRegionsSizeMismatchEmpty();
    MirrorSymmetryScores();
    NormalizeTransformRoundTrip();
    MaskPxToWorldFrontView();
    std::printf("[ok] silhouette kernel tests done\n");
}

} // namespace forge::test
