#include "Silhouette.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace forge {

SilhouetteMask MakeMask(int width, int height)
{
    SilhouetteMask m;
    if (width <= 0 || height <= 0)
        return m;
    m.width = width;
    m.height = height;
    m.pixels.assign((size_t)width * height, 0);
    return m;
}

int MaskArea(const SilhouetteMask& mask)
{
    int area = 0;
    for (uint8_t p : mask.pixels)
        if (p)
            ++area;
    return area;
}

std::optional<mat4> SilhouetteViewProj(const std::string& view, const AABB& bounds)
{
    if (!bounds.Valid())
        return std::nullopt;
    const vec3 c = (bounds.min + bounds.max) * 0.5f;
    // Tiny floor keeps flat targets (a plane seen edge-on) from collapsing the
    // ortho box to zero height.
    const vec3 he = glm::max((bounds.max - bounds.min) * 0.5f, vec3(1e-4f));

    vec3 eye;
    vec3 up{0.0f, 1.0f, 0.0f};
    float hx, hy; // half-extents on the image x/y axes
    if (view == "front") {
        eye = c + vec3(0.0f, 0.0f, he.z * 2.0f + 1.0f);
        hx = he.x; hy = he.y;
    } else if (view == "back") {
        eye = c - vec3(0.0f, 0.0f, he.z * 2.0f + 1.0f);
        hx = he.x; hy = he.y;
    } else if (view == "right") {
        eye = c + vec3(he.x * 2.0f + 1.0f, 0.0f, 0.0f);
        hx = he.z; hy = he.y;
    } else if (view == "left") {
        eye = c - vec3(he.x * 2.0f + 1.0f, 0.0f, 0.0f);
        hx = he.z; hy = he.y;
    } else if (view == "top") {
        eye = c + vec3(0.0f, he.y * 2.0f + 1.0f, 0.0f);
        up = {0.0f, 0.0f, -1.0f}; // plan view: world +z points down the image
        hx = he.x; hy = he.z;
    } else {
        return std::nullopt;
    }

    // Square frame around the larger in-plane extent: nothing clips, and the
    // margin washes out in NormalizeMask anyway. Depth range comfortably
    // covers the whole box from the eye.
    const float half = std::max(hx, hy) * 1.02f;
    const float dist = glm::length(eye - c);
    const mat4 viewM = glm::lookAt(eye, c, up);
    return glm::ortho(-half, half, -half, half, 0.01f, dist + glm::length(he) * 2.0f) * viewM;
}

// Floor division for the subpixel -> pixel bbox (int truncation rounds toward
// zero, which overshoots for negatives).
static int64_t FloorDiv(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

void RasterizeSilhouette(const std::vector<Vertex>& vertices,
                         const std::vector<uint32_t>& indices, const mat4& mvp,
                         SilhouetteMask& mask)
{
    if (mask.width <= 0 || mask.height <= 0)
        return;
    constexpr int64_t kSub = 16; // 1/16-subpixel snap: integer edge math is exact
    // Snapped coordinates clamp to ~±65k px off-frame so int64 edge products
    // can't overflow; the direction error this adds across the frame is under
    // a thousandth of a pixel.
    constexpr int64_t kCoordLimit = int64_t(1) << 20;
    const float w = (float)mask.width, h = (float)mask.height;

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        int64_t px[3], py[3];
        bool ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            const uint32_t idx = indices[i + k];
            if (idx >= vertices.size()) {
                ok = false;
                break;
            }
            const vec4 clip = mvp * vec4(vertices[idx].position, 1.0f);
            if (!(clip.w > 0.0f)) { // behind the eye; ortho projections never trip this
                ok = false;
                break;
            }
            const float ndcX = clip.x / clip.w, ndcY = clip.y / clip.w;
            if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) {
                ok = false;
                break;
            }
            const double fx = (ndcX * 0.5 + 0.5) * w;
            const double fy = (1.0 - (ndcY * 0.5 + 0.5)) * h; // row 0 = top
            px[k] = std::clamp((int64_t)std::llround(fx * (double)kSub), -kCoordLimit, kCoordLimit);
            py[k] = std::clamp((int64_t)std::llround(fy * (double)kSub), -kCoordLimit, kCoordLimit);
        }
        if (!ok)
            continue;

        // Doubled signed area in the y-down pixel frame. Zero = degenerate;
        // negative = flip two vertices so the inclusive >=0 edge test below is
        // the interior regardless of the mesh's winding.
        const int64_t area2 =
            (px[1] - px[0]) * (py[2] - py[0]) - (py[1] - py[0]) * (px[2] - px[0]);
        if (area2 == 0)
            continue;
        if (area2 < 0) {
            std::swap(px[1], px[2]);
            std::swap(py[1], py[2]);
        }

        const int x0 = (int)std::max<int64_t>(0, FloorDiv(std::min({px[0], px[1], px[2]}), kSub));
        const int x1 = (int)std::min<int64_t>(mask.width - 1,
                                              FloorDiv(std::max({px[0], px[1], px[2]}), kSub));
        const int y0 = (int)std::max<int64_t>(0, FloorDiv(std::min({py[0], py[1], py[2]}), kSub));
        const int y1 = (int)std::min<int64_t>(mask.height - 1,
                                              FloorDiv(std::max({py[0], py[1], py[2]}), kSub));

        for (int y = y0; y <= y1; ++y) {
            const int64_t cy = (int64_t)y * kSub + kSub / 2; // pixel centers, GL-style
            uint8_t* row = &mask.pixels[(size_t)y * mask.width];
            for (int x = x0; x <= x1; ++x) {
                const int64_t cx = (int64_t)x * kSub + kSub / 2;
                bool inside = true;
                for (int k = 0; k < 3 && inside; ++k) {
                    const int j = (k + 1) % 3;
                    // Inclusive on every edge — shared edges may double-cover,
                    // which ORing into a binary mask makes harmless. Simpler
                    // and more watertight than a top-left fill rule.
                    inside = (px[j] - px[k]) * (cy - py[k]) - (py[j] - py[k]) * (cx - px[k]) >= 0;
                }
                if (inside)
                    row[x] = 255;
            }
        }
    }
}

// Drop connected components under 1/20 of the largest: compression specks and
// shadow fragments poison the tight-crop bbox, but genuinely separate parts
// (a lid hovering over its jar) stay.
static void DropSpecks(SilhouetteMask& m)
{
    const int w = m.width, h = m.height;
    const int n = w * h;
    std::vector<int32_t> label((size_t)n, -1);
    std::vector<int> areas;
    std::vector<int> stack; // explicit DFS: image-sized recursion would overflow
    for (int i = 0; i < n; ++i) {
        if (!m.pixels[i] || label[i] >= 0)
            continue;
        const int comp = (int)areas.size();
        areas.push_back(0);
        label[i] = comp;
        stack.push_back(i);
        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            ++areas[comp];
            const int x = p % w, y = p / w;
            const int nb[4] = {x > 0 ? p - 1 : -1, x + 1 < w ? p + 1 : -1,
                               y > 0 ? p - w : -1, y + 1 < h ? p + w : -1};
            for (int q : nb)
                if (q >= 0 && m.pixels[q] && label[q] < 0) {
                    label[q] = comp;
                    stack.push_back(q);
                }
        }
    }
    if (areas.empty())
        return;
    const int largest = *std::max_element(areas.begin(), areas.end());
    for (int i = 0; i < n; ++i)
        if (m.pixels[i] && (int64_t)areas[label[i]] * 20 < largest)
            m.pixels[i] = 0;
}

// The flood's gradient gate, shared with enclosed-hole recovery: growth
// crosses smooth shading (vignettes, soft shadows) but stops where adjacent
// pixels step harder than this — JPEG-soft contours still clear it.
static constexpr int kEdgeStep = 5;

// Box-blurred chroma spread: per-pixel max(r,g,b) - min(r,g,b), averaged over
// a clamped 9x9 window. Distance from the gray axis is what separates a
// studio sweep (clinically neutral) from bright object material (warm steel,
// brass reflections) when LUMINANCE cannot — an edge-lit blade rim ramps into
// a white backdrop at under kEdgeStep per pixel for a hundred pixels, so the
// gradient gate alone leaks (#134). Raw per-pixel spread is not usable: JPEG
// chroma subsampling crushes tint in blown highlights and deep shadows; the
// blur recovers a region's tint from its neighbors while true backdrop stays
// at zero. The 4-px reach also wraps a protective band around the object, so
// its blown rim (genuinely neutral in the data) survives with it instead of
// eroding off the silhouette.
static std::vector<uint8_t> BlurredChromaSpread(const uint8_t* rgba, int width, int height)
{
    constexpr int kR = 4; // blur radius; window = (2*kR+1)^2 taps
    constexpr int kTaps = (2 * kR + 1) * (2 * kR + 1);
    const size_t n = (size_t)width * height;
    std::vector<uint8_t> spread(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &rgba[i * 4];
        const uint8_t hi = std::max({p[0], p[1], p[2]});
        const uint8_t lo = std::min({p[0], p[1], p[2]});
        spread[i] = (uint8_t)(hi - lo);
    }
    std::vector<uint8_t> blur(n);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            int sum = 0;
            for (int dy = -kR; dy <= kR; ++dy) {
                const int sy = std::clamp(y + dy, 0, height - 1);
                for (int dx = -kR; dx <= kR; ++dx)
                    sum += spread[(size_t)sy * width + std::clamp(x + dx, 0, width - 1)];
            }
            blur[(size_t)y * width + x] = (uint8_t)((sum + kTaps / 2) / kTaps);
        }
    return blur;
}

// Enclosed through-hole recovery (#134): the border flood cannot reach
// backdrop seen THROUGH the object (an axe head's cutouts), so those pixels
// land in the figure. Re-run the flood's physics from the inside: seed at
// figure pixels that read as SHADOWED BACKDROP — backdrop-neutral chroma and
// between half and four fifths of the border's brightness (a through-hole
// shows the ground occluded and shadowed: meaningfully darker, never brighter,
// and never near-black — β≈0.5..0.8 in shadow-detection terms). Grow with the
// flood's gradient+chroma gates, then punch the region back to background
// only when it
//   (a) is fully enclosed — touches neither the image border nor the outer
//       background (a region the flood merely failed to seed is not a hole);
//   (b) sits between minArea (JPEG salt would pepper the mask with pinhole
//       punches — and no-op on hole-free images must stay bit-identical) and
//       40% of the figure (bigger reads as a mis-flood; the caller's
//       degeneracy checks own that call);
//   (c) has a region MEDIAN inside the shadow window — one bright seed inside
//       noise must not drag a sheen patch through;
//   (d) anchors no foreground island — a shadowed pocket with decoration
//       inside it passes every other test, but punching it would orphan the
//       decoration onto background; a true hole has nothing inside. Only an
//       island wrapped by exactly ONE candidate counts (containment is the
//       evidence): a steel band pinched between two candidate regions is not
//       held by either, and a sub-5%-of-the-candidate island is JPEG noise,
//       not decoration.
// Interior highlights fail structurally: sheen fades smoothly into the
// object, so its region leaks across the whole figure and dies open or over
// the area cap; bright steel pockets fail (c). The whole pass is skipped on
// dark grounds (median < 128): "shadowed backdrop" is unreadable there and
// the window would select object shadow instead.
static void PunchEnclosedHoles(const std::vector<uint8_t>& lum, const std::vector<uint8_t>& sblur,
                               int width, int height, int median, int seedDelta, int punchCap,
                               std::vector<uint8_t>& bg)
{
    if (median < 128)
        return;
    const int shadowLo = median / 2;
    const int shadowHi = median * 4 / 5;
    const int seedHi = median + seedDelta; // growth may climb brighter; seeds may not

    const int n = width * height;
    int64_t fgArea = 0;
    for (int i = 0; i < n; ++i)
        if (!bg[i])
            ++fgArea;
    const int64_t minArea = std::max<int64_t>(9, fgArea / 1000);
    const int64_t maxArea = fgArea * 2 / 5;

    // Pass 1: gradient+chroma-limited regions grown from shadowed-backdrop
    // seeds — the outer flood's growth rules, run from the inside. Per-region
    // luminance histograms feed the median test in the verdict loop.
    struct Candidate {
        int64_t area = 0;
        bool open = false;     // touches the image border or the outer background
        bool anchored = false; // a contained figure island leans on it (pass 3)
        bool punch = false;    // final verdict
        std::array<uint32_t, 256> hist{};
    };
    std::vector<int32_t> region((size_t)n, -1); // -1 = not a hole candidate
    std::vector<Candidate> cands;
    std::vector<int> stack;
    for (int i = 0; i < n; ++i) {
        if (bg[i] || region[i] >= 0 || sblur[i] > punchCap || (int)lum[i] < shadowLo ||
            (int)lum[i] > seedHi)
            continue;
        const int32_t id = (int32_t)cands.size();
        cands.push_back({});
        region[i] = id;
        stack.push_back(i);
        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            ++cands[id].area;
            ++cands[id].hist[lum[p]];
            const int x = p % width, y = p / width;
            if (x == 0 || y == 0 || x == width - 1 || y == height - 1)
                cands[id].open = true;
            const int nb[4] = {x > 0 ? p - 1 : -1, x + 1 < width ? p + 1 : -1,
                               y > 0 ? p - width : -1, y + 1 < height ? p + width : -1};
            for (int q : nb) {
                if (q < 0)
                    continue;
                if (bg[q])
                    cands[id].open = true;
                else if (region[q] < 0 && sblur[q] <= punchCap &&
                         std::abs((int)lum[q] - (int)lum[p]) <= kEdgeStep) {
                    region[q] = id;
                    stack.push_back(q);
                }
            }
        }
    }
    if (cands.empty())
        return;

    // Pass 2: the figure minus every candidate, labeled into components
    // (plain adjacency — object structure, not tone, decides connectivity). A
    // component is grounded when it reaches the outer background or the image
    // border on its own, i.e. it survives any punch.
    struct Component {
        int64_t area = 0;
        bool grounded = false;
        int32_t cand = -1;  // the single adjacent candidate, -1 none yet
        bool multi = false; // touches two or more candidates: contained by none
    };
    std::vector<int32_t> comp((size_t)n, -1);
    std::vector<Component> comps;
    for (int i = 0; i < n; ++i) {
        if (bg[i] || region[i] >= 0 || comp[i] >= 0)
            continue;
        const int32_t id = (int32_t)comps.size();
        comps.push_back({});
        comp[i] = id;
        stack.push_back(i);
        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            ++comps[id].area;
            const int x = p % width, y = p / width;
            if (x == 0 || y == 0 || x == width - 1 || y == height - 1)
                comps[id].grounded = true;
            const int nb[4] = {x > 0 ? p - 1 : -1, x + 1 < width ? p + 1 : -1,
                               y > 0 ? p - width : -1, y + 1 < height ? p + width : -1};
            for (int q : nb) {
                if (q < 0)
                    continue;
                if (bg[q]) {
                    comps[id].grounded = true;
                } else if (region[q] >= 0) {
                    if (comps[id].cand < 0)
                        comps[id].cand = region[q];
                    else if (comps[id].cand != region[q])
                        comps[id].multi = true;
                } else if (comp[q] < 0) {
                    comp[q] = id;
                    stack.push_back(q);
                }
            }
        }
    }

    // Pass 3: an ungrounded island wrapped by exactly one candidate anchors
    // it — unless the island is noise-sized relative to that candidate.
    for (const Component& c : comps)
        if (!c.grounded && !c.multi && c.cand >= 0 &&
            c.area >= std::max<int64_t>(9, cands[c.cand].area / 20))
            cands[c.cand].anchored = true;

    for (Candidate& c : cands) {
        if (c.open || c.anchored || c.area < minArea || c.area > maxArea)
            continue;
        int64_t seen = 0;
        int regionMedian = 0;
        for (int v = 0; v < 256; ++v) {
            seen += c.hist[v];
            if (seen * 2 >= c.area) {
                regionMedian = v;
                break;
            }
        }
        c.punch = regionMedian >= shadowLo && regionMedian <= shadowHi; // test (c)
    }
    for (int i = 0; i < n; ++i)
        if (region[i] >= 0 && cands[region[i]].punch)
            bg[i] = 1;
}

// Background flood from the border: product shots have a ground touching the
// frame edge. Growth is GRADIENT-limited, not tone-limited — the ground is
// smooth (vignettes, soft shadows) while the figure boundary is an edge, so
// the flood rolls across any gradual ground and stops at the object contour.
// That keeps enclosed regions the same tone as the ground (white porcelain on
// a white ground — the case a global threshold fundamentally splits wrong)
// with the object. The absolute tolerance applies only to SEEDING, so a
// figure cropped by the frame edge doesn't seed a leak from inside itself.
// Growth is additionally CHROMA-limited (#134): the flood never enters pixels
// tinted beyond the border's own spread, which is what stops it where the
// gradient gate cannot — an edge-lit steel rim ramping seamlessly into a
// white sweep. On a colored ground the cap derives from the border's spread
// and goes wide, degrading gracefully to the pure gradient behavior.
// Through-holes the flood can't reach are recovered afterwards
// (PunchEnclosedHoles); an alpha matte remains the explicit override.
// False when the result degenerates; caller falls back to Otsu.
static bool BinarizeByBorderFlood(const std::vector<uint8_t>& lum, const std::vector<uint8_t>& sblur,
                                  int width, int height, SilhouetteMask& out)
{
    const int n = width * height;
    std::vector<uint8_t> border, sborder;
    border.reserve((size_t)2 * (width + height));
    sborder.reserve((size_t)2 * (width + height));
    auto sample = [&](size_t i) {
        border.push_back(lum[i]);
        sborder.push_back(sblur[i]);
    };
    for (int x = 0; x < width; ++x) {
        sample((size_t)x);
        sample((size_t)(height - 1) * width + x);
    }
    for (int y = 1; y + 1 < height; ++y) {
        sample((size_t)y * width);
        sample((size_t)y * width + width - 1);
    }
    if (border.empty())
        return false;
    auto medianOf = [](std::vector<uint8_t>& v) {
        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return (int)v[v.size() / 2];
    };
    auto madOf = [&](std::vector<uint8_t>& v, int med) {
        std::vector<uint8_t> dev(v.size());
        for (size_t i = 0; i < v.size(); ++i)
            dev[i] = (uint8_t)std::abs((int)v[i] - med);
        return medianOf(dev);
    };
    const int median = medianOf(border);
    const int mad = madOf(border, median);
    const int seedDelta = std::max(24, 3 * mad);
    const int smedian = medianOf(sborder);
    const int smad = madOf(sborder, smedian);
    // The chroma model only speaks when the border says the ground is
    // NEUTRAL: on a colored ground the spread of object material sits inside
    // the ground's own spread, the gate would flap on JPEG chroma noise, and
    // hole-free references must stay bit-identical to the pre-#134 flood.
    // Flood gate strict (a leak erases whole object regions); punch gate loose
    // (JPEG mosquito noise inside a real hole must not fragment it — the punch
    // has four other guards).
    const bool neutralGround = smedian <= 8;
    const int floodCap = neutralGround ? smedian + std::max(2, 3 * smad) : 255;
    const int punchCap = neutralGround ? smedian + std::max(8, 3 * smad) : 255;

    std::vector<uint8_t> bg((size_t)n, 0);
    std::vector<int> stack;
    auto seed = [&](int i) {
        if (!bg[i] && std::abs((int)lum[i] - median) <= seedDelta && sblur[i] <= floodCap) {
            bg[i] = 1;
            stack.push_back(i);
        }
    };
    for (int x = 0; x < width; ++x) {
        seed(x);
        seed((height - 1) * width + x);
    }
    for (int y = 1; y + 1 < height; ++y) {
        seed(y * width);
        seed(y * width + width - 1);
    }
    int bgCount = (int)stack.size();
    while (!stack.empty()) {
        const int p = stack.back();
        stack.pop_back();
        const int x = p % width, y = p / width;
        const int nb[4] = {x > 0 ? p - 1 : -1, x + 1 < width ? p + 1 : -1,
                           y > 0 ? p - width : -1, y + 1 < height ? p + width : -1};
        for (int q : nb)
            if (q >= 0 && !bg[q] && sblur[q] <= floodCap &&
                std::abs((int)lum[q] - (int)lum[p]) <= kEdgeStep) {
                bg[q] = 1;
                stack.push_back(q);
                ++bgCount;
            }
    }
    const int fg = n - bgCount;
    // 64-bit compare: stbi admits images past ~22.6 MP, where n * 95 overflows
    // int. The max(1, ...) floor keeps a fully-flooded tiny image degenerate
    // (fg = 0 must fall back to Otsu, not report an empty success).
    if (fg < std::max(1, n / 100) || (int64_t)fg * 100 > (int64_t)n * 95)
        return false; // flood leaked through the figure, or found no ground

    // After the degeneracy verdict, not before: recovery only ever shrinks the
    // figure, and a mask that needed the Otsu fallback should not be rescued
    // into a different segmentation by hole-punching.
    PunchEnclosedHoles(lum, sblur, width, height, median, seedDelta, punchCap, bg);

    out = MakeMask(width, height);
    for (int i = 0; i < n; ++i)
        out.pixels[i] = bg[i] ? 0 : 255;
    return true;
}

SilhouetteMask BinarizeImage(const uint8_t* rgba, int width, int height)
{
    if (!rgba || width <= 0 || height <= 0)
        return {};
    SilhouetteMask m = MakeMask(width, height);
    const size_t n = (size_t)width * height;

    // Alpha carries information when anything is meaningfully transparent —
    // then it IS the matte (product-shot PNGs). Opaque images (and JPEGs,
    // which decode with alpha 255 everywhere) fall through to luminance.
    bool hasMatte = false;
    for (size_t i = 0; i < n && !hasMatte; ++i)
        hasMatte = rgba[i * 4 + 3] < 250;
    if (hasMatte) {
        for (size_t i = 0; i < n; ++i)
            m.pixels[i] = rgba[i * 4 + 3] >= 128 ? 255 : 0;
        // No polarity guess needed, but speck cleanup still is: web cutouts
        // carry orphan alpha dust, and one pixel poisons the crop box.
        DropSpecks(m);
        return m;
    }

    std::vector<uint8_t> lum(n);
    uint32_t hist[256] = {};
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &rgba[i * 4];
        lum[i] = (uint8_t)((299 * p[0] + 587 * p[1] + 114 * p[2]) / 1000);
        ++hist[lum[i]];
    }

    const std::vector<uint8_t> sblur = BlurredChromaSpread(rgba, width, height);
    if (BinarizeByBorderFlood(lum, sblur, width, height, m)) {
        DropSpecks(m);
        return m;
    }

    // Otsu fallback: the threshold maximizing between-class variance of the
    // luminance histogram — solid on bimodal images where the flood's
    // uniform-ground assumption doesn't hold.
    const double total = (double)n;
    double sumAll = 0.0;
    for (int i = 0; i < 256; ++i)
        sumAll += (double)i * hist[i];
    double sumB = 0.0, wB = 0.0, best = -1.0;
    int threshold = 0;
    for (int i = 0; i < 256; ++i) {
        wB += hist[i];
        if (wB == 0.0)
            continue;
        const double wF = total - wB;
        if (wF == 0.0)
            break;
        sumB += (double)i * hist[i];
        const double mB = sumB / wB, mF = (sumAll - sumB) / wF;
        const double between = wB * wF * (mB - mF) * (mB - mF);
        if (between > best) {
            best = between;
            threshold = i;
        }
    }

    // Otsu doesn't know which class is the object. Product shots have a
    // background border: whichever class the border majority lands in is
    // background, the other is foreground.
    int64_t borderAbove = 0, borderTotal = 0;
    auto tally = [&](size_t i) {
        ++borderTotal;
        if (lum[i] > threshold)
            ++borderAbove;
    };
    for (int x = 0; x < width; ++x) {
        tally((size_t)x);
        tally((size_t)(height - 1) * width + x);
    }
    for (int y = 1; y + 1 < height; ++y) {
        tally((size_t)y * width);
        tally((size_t)y * width + width - 1);
    }
    const bool foregroundAbove = borderAbove * 2 <= borderTotal;
    for (size_t i = 0; i < n; ++i) {
        const bool above = lum[i] > threshold;
        m.pixels[i] = above == foregroundAbove ? 255 : 0;
    }
    DropSpecks(m);
    return m;
}

SilhouetteMask NormalizeMask(const SilhouetteMask& in, int outSize)
{
    SilhouetteMask out = MakeMask(outSize, outSize);
    if (in.width <= 0 || in.height <= 0 || outSize <= 0)
        return out;

    int minX = in.width, minY = in.height, maxX = -1, maxY = -1;
    for (int y = 0; y < in.height; ++y)
        for (int x = 0; x < in.width; ++x)
            if (in.pixels[(size_t)y * in.width + x]) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
            }
    if (maxX < 0)
        return out; // nothing covered

    const int bw = maxX - minX + 1, bh = maxY - minY + 1;
    // Larger side fills the frame exactly; the other scales uniformly — never
    // stretch axes independently, aspect ratio IS shape information.
    int ow, oh;
    if (bw >= bh) {
        ow = outSize;
        oh = std::max(1, (int)std::lround((double)bh * outSize / bw));
    } else {
        oh = outSize;
        ow = std::max(1, (int)std::lround((double)bw * outSize / bh));
    }
    const int offX = (outSize - ow) / 2, offY = (outSize - oh) / 2;
    for (int y = 0; y < oh; ++y) {
        const int sy = minY + (int)((int64_t)y * bh / oh); // nearest sample
        for (int x = 0; x < ow; ++x) {
            const int sx = minX + (int)((int64_t)x * bw / ow);
            if (in.pixels[(size_t)sy * in.width + sx])
                out.pixels[(size_t)(y + offY) * outSize + (x + offX)] = 255;
        }
    }
    return out;
}

SilhouetteDiff CompareMasks(const SilhouetteMask& a, const SilhouetteMask& b)
{
    SilhouetteDiff d;
    if (a.width != b.width || a.height != b.height || a.width <= 0 || a.height <= 0)
        return d;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const bool sa = a.pixels[i] != 0, sb = b.pixels[i] != 0;
        if (sa && sb)
            ++d.intersection;
        else if (sa)
            ++d.onlyA;
        else if (sb)
            ++d.onlyB;
    }
    d.unionArea = d.intersection + d.onlyA + d.onlyB;
    // Empty-vs-empty scores 0, not 1: "no shape" never verifies a shape claim.
    if (d.unionArea > 0)
        d.iou = (float)d.intersection / (float)d.unionArea;
    const int total = 2 * d.intersection + d.onlyA + d.onlyB;
    if (total > 0)
        d.dice = 2.0f * (float)d.intersection / (float)total;
    return d;
}

std::vector<uint8_t> DiffImageRGBA(const SilhouetteMask& a, const SilhouetteMask& b)
{
    if (a.width != b.width || a.height != b.height || a.width <= 0 || a.height <= 0)
        return {};
    std::vector<uint8_t> img((size_t)a.width * a.height * 4);
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const bool sa = a.pixels[i] != 0, sb = b.pixels[i] != 0;
        uint8_t r = 18, g = 18, bl = 18; // background
        if (sa && sb) {
            r = g = bl = 225; // agreement
        } else if (sa) {
            r = 40; g = 220; bl = 80; // render only
        } else if (sb) {
            r = 230; g = 60; bl = 230; // reference only
        }
        img[i * 4 + 0] = r;
        img[i * 4 + 1] = g;
        img[i * 4 + 2] = bl;
        img[i * 4 + 3] = 255;
    }
    return img;
}

} // namespace forge
