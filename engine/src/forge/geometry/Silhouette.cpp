#include "Silhouette.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <queue>

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
    // Separable sliding-window box sums (edge-replicated) — bit-identical to
    // the naive (2R+1)^2 gather but O(n): this runs on the GL main thread for
    // every opaque reference load, and 81 taps per pixel measured a 7x
    // binarize slowdown. Row sums cap at 9*255 = 2295 (uint16); column sums
    // of those cap well inside int.
    std::vector<uint16_t> rowSum(n);
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = &spread[(size_t)y * width];
        uint16_t* out = &rowSum[(size_t)y * width];
        int sum = 0;
        for (int dx = -kR; dx <= kR; ++dx)
            sum += row[std::clamp(dx, 0, width - 1)];
        out[0] = (uint16_t)sum;
        for (int x = 1; x < width; ++x) {
            sum += row[std::clamp(x + kR, 0, width - 1)] - row[std::clamp(x - kR - 1, 0, width - 1)];
            out[x] = (uint16_t)sum;
        }
    }
    std::vector<uint8_t> blur(n);
    for (int x = 0; x < width; ++x) {
        int sum = 0;
        for (int dy = -kR; dy <= kR; ++dy)
            sum += rowSum[(size_t)std::clamp(dy, 0, height - 1) * width + x];
        blur[x] = (uint8_t)((sum + kTaps / 2) / kTaps);
        for (int y = 1; y < height; ++y) {
            sum += rowSum[(size_t)std::clamp(y + kR, 0, height - 1) * width + x] -
                   rowSum[(size_t)std::clamp(y - kR - 1, 0, height - 1) * width + x];
            blur[(size_t)y * width + x] = (uint8_t)((sum + kTaps / 2) / kTaps);
        }
    }
    return blur;
}

// Enclosed through-hole recovery (#134): the border flood cannot reach
// backdrop seen THROUGH the object (an axe head's cutouts), so those pixels
// land in the figure. Re-run the flood's physics from the inside: seed at
// figure pixels with backdrop-neutral chroma anywhere above half the border's
// brightness (the wide top is load-bearing — a sheen region must swallow its
// whole bright ramp so it dies open or over-cap), grow with the flood's
// gradient+chroma gates, then apply the SHADOW test to the region's median in
// verdict (c): a through-hole shows the ground occluded and shadowed, between
// half and four fifths of the border's brightness — meaningfully darker,
// never brighter, never near-black (β≈0.5..0.8 in shadow-detection terms).
// Punch the region back to background only when it
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
// the window would select object shadow instead. The CALLER additionally
// skips it on non-neutral grounds, where punchCap sits at 255 and the chroma
// guard — the only thing keeping tinted recesses safe — would be inert.
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

    // Pass 1: gradient+chroma-limited regions grown from backdrop-toned
    // seeds — the outer flood's growth rules, run from the inside. Candidates
    // must stay a few flags each: the seed window accepts most mid-to-bright
    // figure pixels, so a grainy grayscale figure can fragment into a
    // candidate per pixel (a 4 MP reference measured 2 GB with a 1 KB
    // histogram embedded per candidate).
    struct Candidate {
        int64_t area = 0;
        bool open = false;     // touches the image border or the outer background
        bool anchored = false; // a contained figure island leans on it (pass 3)
        bool punch = false;    // final verdict
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

    // Verdict: cheap gates first; region medians only for the few candidates
    // that survive them, with the 256-bin histograms accumulated in one extra
    // sweep over just those regions.
    std::vector<int32_t> shortIdx(cands.size(), -1);
    int32_t shortCount = 0;
    for (size_t c = 0; c < cands.size(); ++c)
        if (!cands[c].open && !cands[c].anchored && cands[c].area >= minArea &&
            cands[c].area <= maxArea)
            shortIdx[c] = shortCount++;
    if (shortCount == 0)
        return;
    std::vector<std::array<uint32_t, 256>> hists((size_t)shortCount); // zeroed
    for (int i = 0; i < n; ++i)
        if (region[i] >= 0 && shortIdx[region[i]] >= 0)
            ++hists[shortIdx[region[i]]][lum[i]];
    for (size_t c = 0; c < cands.size(); ++c) {
        if (shortIdx[c] < 0)
            continue;
        const std::array<uint32_t, 256>& h = hists[shortIdx[c]];
        int64_t seen = 0;
        int regionMedian = 0;
        for (int v = 0; v < 256; ++v) {
            seen += h[v];
            if (seen * 2 >= cands[c].area) {
                regionMedian = v;
                break;
            }
        }
        cands[c].punch = regionMedian >= shadowLo && regionMedian <= shadowHi; // test (c)
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
    // into a different segmentation by hole-punching. Neutral grounds only:
    // with punchCap at 255 the punch would run on luminance alone and eat
    // tinted recesses — colored grounds keep pre-#134 behavior exactly, and
    // holes there remain the documented alpha-matte case.
    if (neutralGround)
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

    // Two-valued gray fast path (#135): a silhouette PNG round-tripped as a
    // reference is opaque, r==g==b, and holds exactly the figure/background
    // pair. Segment it by border majority instead of flooding — the dark-ground
    // flood below would weld the axe's enclosed cutouts shut (PunchEnclosedHoles
    // bails on a dark median). JPEG chroma noise alone spreads r,g,b past this
    // test, so no photograph reaches it; a same-tone enclosed region needs a
    // third tone (the outline) to survive the flood, so that case is >= 3-valued
    // and untouched here.
    {
        bool twoValued = true;
        int v0 = -1, v1 = -1; // the (at most) two distinct gray levels seen
        for (size_t i = 0; i < n && twoValued; ++i) {
            const uint8_t* p = &rgba[i * 4];
            if (p[0] != p[1] || p[1] != p[2]) { // any chroma disqualifies
                twoValued = false;
                break;
            }
            const int g = p[0];
            if (v0 < 0 || g == v0)
                v0 = g;
            else if (v1 < 0 || g == v1)
                v1 = g;
            else
                twoValued = false; // a third distinct value: not a silhouette PNG
        }
        if (twoValued && (v1 < 0 || std::abs(v1 - v0) >= 128)) {
            if (v1 < 0)
                return m; // single flat color: nothing to segment
            // Border majority is the ground; the other value is the figure.
            int64_t borderV0 = 0, borderTotal = 0;
            auto tally = [&](size_t i) {
                ++borderTotal;
                if ((int)rgba[i * 4] == v0)
                    ++borderV0;
            };
            for (int x = 0; x < width; ++x) {
                tally((size_t)x);
                tally((size_t)(height - 1) * width + x);
            }
            for (int y = 1; y + 1 < height; ++y) {
                tally((size_t)y * width);
                tally((size_t)y * width + width - 1);
            }
            const int fgValue = borderV0 * 2 >= borderTotal ? v1 : v0;
            for (size_t i = 0; i < n; ++i)
                m.pixels[i] = (int)rgba[i * 4] == fgValue ? 255 : 0;
            DropSpecks(m); // shared tail: orphan specks still poison the crop box
            return m;
        }
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

SilhouetteMask NormalizeMask(const SilhouetteMask& in, int outSize,
                             NormalizeTransform& xform)
{
    xform = NormalizeTransform{}; // stale caller state must not survive an early-out
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
    // Record the crop+scale+pad so callers can map normalized pixels back to
    // source pixels (#136) — NormalizedToSourcePx is this framing's inverse.
    xform.srcMinX = minX;
    xform.srcMinY = minY;
    xform.srcW = bw;
    xform.srcH = bh;
    xform.scaledW = ow;
    xform.scaledH = oh;
    xform.padX = offX;
    xform.padY = offY;
    xform.valid = true;
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

SilhouetteMask NormalizeMask(const SilhouetteMask& in, int outSize)
{
    NormalizeTransform xform; // discarded: shape-only callers don't need the framing
    return NormalizeMask(in, outSize, xform);
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

// --- structured diff (#136): mismatch regions as data ------------------------

DiffRegionList DiffRegions(const SilhouetteMask& a, const SilhouetteMask& b,
                           int maxRegions, float minAreaFraction)
{
    DiffRegionList out;
    if (a.width != b.width || a.height != b.height || a.width <= 0 || a.height <= 0)
        return out; // mirrors CompareMasks: mismatched frames carry no diff
    maxRegions = std::max(0, maxRegions);
    minAreaFraction = std::max(0.0f, minAreaFraction);

    const int w = a.width, h = a.height;
    const int n = w * h;
    // One pass classifies every pixel (0 = agree, 1 = A-only, 2 = B-only) and
    // tallies the union, so areaFraction shares IoU's denominator without a
    // second CompareMasks sweep.
    std::vector<uint8_t> cls((size_t)n, 0);
    int unionArea = 0;
    for (int i = 0; i < n; ++i) {
        const bool sa = a.pixels[i] != 0, sb = b.pixels[i] != 0;
        if (sa || sb)
            ++unionArea;
        if (sa != sb)
            cls[i] = sa ? 1 : 2;
    }
    if (unionArea == 0)
        return out; // two empty masks agree everywhere: no regions to report

    // Flood fill, 4-connected, same-class only (excess and missing never join
    // across a shared border). Explicit stack: a snake region in a 1024^2 mask
    // would blow the call stack under recursion. Pixels are consumed (class
    // cleared) as they are pushed, so labeling stays one O(W*H) pass total.
    std::vector<DiffRegion> found;
    std::vector<int> stack;
    for (int i = 0; i < n; ++i) {
        const uint8_t regionClass = cls[i];
        if (regionClass == 0)
            continue;
        DiffRegion r;
        r.excess = regionClass == 1;
        r.minX = w;
        r.minY = h;
        r.maxX = -1;
        r.maxY = -1;
        double sumX = 0.0, sumY = 0.0; // pixel-center sums, like MaskLandmarks
        cls[i] = 0;
        stack.push_back(i);
        while (!stack.empty()) {
            const int p = stack.back();
            stack.pop_back();
            const int x = p % w, y = p / w;
            ++r.area;
            sumX += (double)x + 0.5;
            sumY += (double)y + 0.5;
            r.minX = std::min(r.minX, x);
            r.maxX = std::max(r.maxX, x);
            r.minY = std::min(r.minY, y);
            r.maxY = std::max(r.maxY, y);
            const int nb[4] = {x > 0 ? p - 1 : -1, x + 1 < w ? p + 1 : -1,
                               y > 0 ? p - w : -1, y + 1 < h ? p + w : -1};
            for (int q : nb)
                if (q >= 0 && cls[q] == regionClass) {
                    cls[q] = 0;
                    stack.push_back(q);
                }
        }
        r.areaFraction = (float)r.area / (float)unionArea;
        r.centroid = vec2((float)(sumX / (double)r.area), (float)(sumY / (double)r.area));
        found.push_back(r);
    }
    out.totalRegions = (int)found.size();

    // Speck floor first, cap second — the counters account for both, so a
    // truncated list never silently reads as complete coverage.
    std::vector<DiffRegion> kept;
    kept.reserve(found.size());
    for (const DiffRegion& r : found) {
        if (r.areaFraction < minAreaFraction) {
            ++out.droppedRegions;
            out.droppedArea += r.area;
        } else {
            kept.push_back(r);
        }
    }
    // Stable sort: regions tying on area/minY/minX keep raster-scan discovery
    // order, so the emitted list is fully deterministic for tests.
    std::stable_sort(kept.begin(), kept.end(),
                     [](const DiffRegion& lhs, const DiffRegion& rhs) {
                         if (lhs.area != rhs.area)
                             return lhs.area > rhs.area;
                         if (lhs.minY != rhs.minY)
                             return lhs.minY < rhs.minY;
                         return lhs.minX < rhs.minX;
                     });
    while ((int)kept.size() > maxRegions) {
        ++out.droppedRegions;
        out.droppedArea += kept.back().area;
        kept.pop_back();
    }
    out.regions = std::move(kept);
    return out;
}

float MaskMirrorSymmetryX(const SilhouetteMask& mask)
{
    const int w = mask.width, h = mask.height;
    if (w <= 0 || h <= 0 || (size_t)w * h != mask.pixels.size())
        return 0.0f;
    int minX = w, maxX = -1;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &mask.pixels[(size_t)y * w];
        for (int x = 0; x < w; ++x)
            if (row[x]) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
            }
    }
    if (maxX < 0)
        return 0.0f; // no shape is not a symmetric shape (matches CompareMasks' empty rule)

    // x -> minX+maxX-x maps [minX, maxX] onto itself, so |mirror(M)| = |M| and
    // union = 2*area - intersection without materializing the mirrored mask.
    // int64 counters: 2*area on a many-megapixel reference mask would sit
    // uncomfortably close to int range (the MaskLandmarks #114 lesson).
    int64_t area = 0, inter = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &mask.pixels[(size_t)y * w];
        for (int x = minX; x <= maxX; ++x)
            if (row[x]) {
                ++area;
                if (row[minX + maxX - x])
                    ++inter;
            }
    }
    return (float)inter / (float)(2 * area - inter); // area >= 1, denominator > 0
}

vec2 NormalizedToSourcePx(const NormalizeTransform& xform, vec2 normPx)
{
    // scaledW/scaledH are >= 1 whenever valid; the extra check keeps a
    // hand-built transform from dividing by zero.
    if (!xform.valid || xform.scaledW <= 0 || xform.scaledH <= 0)
        return normPx;
    return {(float)xform.srcMinX + (normPx.x - (float)xform.padX) * (float)xform.srcW /
                                       (float)xform.scaledW,
            (float)xform.srcMinY + (normPx.y - (float)xform.padY) * (float)xform.srcH /
                                       (float)xform.scaledH};
}

vec3 MaskPxToWorld(const mat4& viewProj, int width, int height, vec2 px, float ndcZ)
{
    const float ndcX = 2.0f * px.x / (float)width - 1.0f;
    const float ndcY = 1.0f - 2.0f * px.y / (float)height; // row 0 = top, as rasterized
    // No cached inverse: callers map a handful of points per compare.
    const vec4 world = glm::inverse(viewProj) * vec4(ndcX, ndcY, ndcZ, 1.0f);
    return vec3(world) / world.w; // ortho keeps w = 1; dividing is free correctness
}

// --- multi-view (#137): combined gate over per-view scores ---------------------

ViewScoreSummary CombineViewScores(const std::vector<float>& ious, float threshold)
{
    ViewScoreSummary s;
    if (ious.empty())
        return s; // zeroed defaults: no views never verifies a shape claim
    s.minIou = ious[0];
    s.allPass = true;
    double sum = 0.0; // double accumulation, one cast at the end
    for (const float iou : ious) {
        s.minIou = std::min(s.minIou, iou);
        sum += (double)iou;
        if (!(iou >= threshold)) // >= : exactly-at-threshold passes, like the single view
            s.allPass = false;
    }
    s.meanIou = (float)(sum / (double)ious.size());
    return s;
}

// --- outline extraction (#135) ------------------------------------------------

double PolygonSignedArea(const std::vector<vec2>& points)
{
    const size_t n = points.size();
    if (n < 3)
        return 0.0;
    double a2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const vec2& p = points[i];
        const vec2& q = points[(i + 1) % n];
        a2 += (double)p.x * (double)q.y - (double)q.x * (double)p.y;
    }
    return 0.5 * a2;
}

vec2 PolygonCentroid(const std::vector<vec2>& points)
{
    const size_t n = points.size();
    if (n == 0)
        return vec2(0.0f);
    double a2 = 0.0, cx = 0.0, cy = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const vec2& p = points[i];
        const vec2& q = points[(i + 1) % n];
        const double cross = (double)p.x * (double)q.y - (double)q.x * (double)p.y;
        a2 += cross;
        cx += ((double)p.x + (double)q.x) * cross;
        cy += ((double)p.y + (double)q.y) * cross;
    }
    // The shoelace centroid divides by area; a collinear sliver (area ~ 0) would
    // explode, so fall back to the vertex mean there.
    if (std::abs(a2) < 1e-9) {
        double mx = 0.0, my = 0.0;
        for (const vec2& p : points) {
            mx += p.x;
            my += p.y;
        }
        return vec2((float)(mx / (double)n), (float)(my / (double)n));
    }
    return vec2((float)(cx / (3.0 * a2)), (float)(cy / (3.0 * a2)));
}

// Even-odd point-in-polygon (PNPOLY). Callers pass pixel CENTERS (half-integer)
// against integer contour vertices, so the horizontal ray never grazes a vertex
// — there is no on-edge degeneracy to special-case, and a saddle's self-touching
// loop still tests correctly (even-odd counts net crossings).
static bool PointInPolygon(const vec2& pt, const std::vector<vec2>& poly)
{
    const size_t n = poly.size();
    if (n < 3)
        return false;
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const vec2& a = poly[i];
        const vec2& b = poly[j];
        if ((a.y > pt.y) != (b.y > pt.y)) {
            const double xCross =
                (double)a.x + (double)(pt.y - a.y) / (double)(b.y - a.y) *
                                  ((double)b.x - (double)a.x);
            if ((double)pt.x < xCross)
                inside = !inside;
        }
    }
    return inside;
}

std::vector<SilhouetteContour> TraceContours(const SilhouetteMask& mask, double minArea)
{
    std::vector<SilhouetteContour> contours;
    const int w = mask.width, h = mask.height;
    if (w <= 0 || h <= 0 || (size_t)w * h != mask.pixels.size())
        return contours;

    auto covered = [&](int x, int y) -> bool {
        return x >= 0 && y >= 0 && x < w && y < h && mask.pixels[(size_t)y * w + x] != 0;
    };
    const int cw = w + 1; // corner-lattice width
    auto corner = [&](int cx, int cy) { return cy * cw + cx; };

    // Directed boundary edges on the corner lattice. Per covered pixel, an edge
    // is emitted along each side facing an uncovered neighbor, oriented so the
    // covered region stays on a consistent side: outer loops then come out with
    // POSITIVE shoelace area in the y-down frame and holes NEGATIVE, by
    // construction rather than a post-hoc sign flip.
    struct Edge {
        int start, end;  // corner indices
        int emitPix;     // the covered pixel that emitted this edge (always valid)
        int uncovPix;    // the uncovered neighbor across the edge, -1 if off-image
    };
    std::vector<Edge> edges;
    // A corner has two outgoing edges only at a saddle; a fixed 2-slot table
    // per corner avoids a per-corner heap allocation on megapixel masks.
    std::vector<std::array<int, 2>> outSlot((size_t)cw * (h + 1), {-1, -1});
    auto addEdge = [&](int s, int e, int emit, int uncov) {
        const int idx = (int)edges.size();
        edges.push_back({s, e, emit, uncov});
        std::array<int, 2>& slot = outSlot[(size_t)s];
        slot[slot[0] < 0 ? 0 : 1] = idx;
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!covered(x, y))
                continue;
            const int pix = y * w + x;
            if (!covered(x, y - 1)) // N: top edge, left -> right
                addEdge(corner(x, y), corner(x + 1, y), pix, y > 0 ? (y - 1) * w + x : -1);
            if (!covered(x + 1, y)) // E: right edge, top -> bottom
                addEdge(corner(x + 1, y), corner(x + 1, y + 1), pix,
                        x + 1 < w ? y * w + x + 1 : -1);
            if (!covered(x, y + 1)) // S: bottom edge, right -> left
                addEdge(corner(x + 1, y + 1), corner(x, y + 1), pix,
                        y + 1 < h ? (y + 1) * w + x : -1);
            if (!covered(x - 1, y)) // W: left edge, bottom -> top
                addEdge(corner(x, y + 1), corner(x, y), pix, x > 0 ? y * w + x - 1 : -1);
        }

    std::vector<int> reps; // representative interior pixel per contour, -1 = none
    std::vector<char> used(edges.size(), 0);
    for (int startEdge = 0; startEdge < (int)edges.size(); ++startEdge) {
        if (used[(size_t)startEdge])
            continue;
        std::vector<int> loopCorners; // start corners in walk order
        int coveredPix = -1, uncovPix = -1;
        int e = startEdge;
        while (e >= 0 && !used[(size_t)e]) {
            used[(size_t)e] = 1;
            loopCorners.push_back(edges[(size_t)e].start);
            if (coveredPix < 0)
                coveredPix = edges[(size_t)e].emitPix;
            if (uncovPix < 0 && edges[(size_t)e].uncovPix >= 0)
                uncovPix = edges[(size_t)e].uncovPix;
            const int c = edges[(size_t)e].end;
            const std::array<int, 2>& slot = outSlot[(size_t)c];
            int next = -1;
            if (slot[1] < 0) {
                next = (slot[0] >= 0 && !used[(size_t)slot[0]]) ? slot[0] : -1;
            } else {
                // Saddle (two diagonal covered squares meet at c): cross to the
                // OTHER square so the two stay on ONE 8-connected loop. Round-trip
                // is exact either way; the checkerboard test pins this choice.
                for (int k = 0; k < 2; ++k) {
                    const int cand = slot[k];
                    if (cand >= 0 && !used[(size_t)cand] &&
                        edges[(size_t)cand].emitPix != edges[(size_t)e].emitPix)
                        next = cand;
                }
                if (next < 0) // second visit: only the same-square edge remains
                    for (int k = 0; k < 2; ++k)
                        if (slot[k] >= 0 && !used[(size_t)slot[k]])
                            next = slot[k];
            }
            e = next;
        }

        // Corner indices -> lattice coords, then merge collinear runs. Crack
        // following emits a vertex every unit step; dropping the middle of a
        // collinear triple makes a rect 4 points and cuts memory ~10x on big
        // masks, with no effect on the traced segment (or on exactness).
        const size_t rn = loopCorners.size();
        std::vector<vec2> pts;
        for (size_t i = 0; i < rn; ++i) {
            const int cPrev = loopCorners[(i + rn - 1) % rn];
            const int cCur = loopCorners[i];
            const int cNext = loopCorners[(i + 1) % rn];
            const vec2 prev((float)(cPrev % cw), (float)(cPrev / cw));
            const vec2 cur((float)(cCur % cw), (float)(cCur / cw));
            const vec2 next((float)(cNext % cw), (float)(cNext / cw));
            const double cross = (double)(cur.x - prev.x) * (double)(next.y - prev.y) -
                                 (double)(cur.y - prev.y) * (double)(next.x - prev.x);
            if (cross != 0.0)
                pts.push_back(cur);
        }
        if (pts.size() < 3)
            continue; // degenerate stub

        SilhouetteContour contour;
        contour.area = PolygonSignedArea(pts);
        contour.hole = contour.area < 0.0;
        contour.points = std::move(pts);
        // Representative interior pixel for the parent test: a covered pixel for
        // an outer loop (inside the blob), the uncovered neighbor for a hole
        // (inside the hole). Its center is half-integer -> exact even-odd test.
        reps.push_back(contour.hole ? uncovPix : coveredPix);
        contours.push_back(std::move(contour));
    }

    // Area floor: drop sub-minArea loops (and their parallel reps) BEFORE the
    // quadratic parent pass — the caller opted into losing pinholes it cannot
    // use in exchange for bounded work here.
    if (minArea > 0.0) {
        size_t keep = 0;
        for (size_t i = 0; i < contours.size(); ++i)
            if (std::abs(contours[i].area) >= minArea) {
                if (keep != i) {
                    contours[keep] = std::move(contours[i]);
                    reps[keep] = reps[i];
                }
                ++keep;
            }
        contours.resize(keep);
        reps.resize(keep);
    }

    // Immediate parent = the smallest-|area| OTHER contour whose polygon
    // contains this contour's representative center. DropSpecks removes only
    // COVERED specks — uncovered pinholes survive it and can number in the
    // thousands on a grainy mask — so this quadratic pass is bounded by the
    // caller's minArea floor plus the bbox prefilter below, not by any
    // "loops are few" assumption.
    std::vector<vec2> bbLo(contours.size()), bbHi(contours.size());
    for (size_t i = 0; i < contours.size(); ++i) {
        vec2 lo = contours[i].points[0], hi = lo;
        for (const vec2& p : contours[i].points) {
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
        bbLo[i] = lo;
        bbHi[i] = hi;
    }
    for (size_t i = 0; i < contours.size(); ++i) {
        const int rp = reps[i];
        if (rp < 0)
            continue;
        const vec2 center((float)(rp % w) + 0.5f, (float)(rp / w) + 0.5f);
        double bestArea = 0.0;
        int best = -1;
        for (size_t j = 0; j < contours.size(); ++j) {
            if (j == i)
                continue;
            // Bbox reject first: a point inside a polygon is inside its bbox,
            // so this is pure pruning — PNPOLY only runs on real candidates.
            if (center.x < bbLo[j].x || center.x > bbHi[j].x || center.y < bbLo[j].y ||
                center.y > bbHi[j].y)
                continue;
            if (!PointInPolygon(center, contours[j].points))
                continue;
            const double aj = std::abs(contours[j].area);
            if (best < 0 || aj < bestArea) {
                bestArea = aj;
                best = (int)j;
            }
        }
        contours[i].parent = best;
    }
    return contours;
}

std::vector<vec2> SimplifyContour(const std::vector<vec2>& points, int maxPoints, float tolerance)
{
    const int n = (int)points.size();
    if (n < 3)
        return points;
    maxPoints = std::max(3, maxPoints);

    // Closed-loop anchors: bottom-most vertex, then the vertex farthest from it.
    // Two chains between them seed the priority split.
    int a0 = 0;
    for (int i = 1; i < n; ++i)
        if (points[i].y > points[a0].y ||
            (points[i].y == points[a0].y && points[i].x > points[a0].x))
            a0 = i;
    int a1 = a0;
    double bestD = -1.0;
    for (int i = 0; i < n; ++i) {
        const double dx = (double)points[i].x - points[a0].x;
        const double dy = (double)points[i].y - points[a0].y;
        const double d = dx * dx + dy * dy;
        if (d > bestD) {
            bestD = d;
            a1 = i;
        }
    }
    if (a1 == a0)
        return points; // all vertices coincide

    // Point-to-SEGMENT distance (t clamped): the endpoints of a near-degenerate
    // chord must not pull the deviation off to infinity like an infinite line.
    auto segDist = [&](const vec2& p, const vec2& a, const vec2& b) -> double {
        const double vx = (double)b.x - a.x, vy = (double)b.y - a.y;
        const double wx = (double)p.x - a.x, wy = (double)p.y - a.y;
        const double len2 = vx * vx + vy * vy;
        double t = len2 > 0.0 ? (wx * vx + wy * vy) / len2 : 0.0;
        t = std::clamp(t, 0.0, 1.0);
        const double dx = wx - t * vx, dy = wy - t * vy;
        return std::sqrt(dx * dx + dy * dy);
    };
    // Farthest interior ring position strictly between i and j (walking forward
    // mod n) and its distance to the chord. Lowest step wins ties (strict >).
    auto farthest = [&](int i, int j, int& outPos, double& outDist) {
        outPos = -1;
        outDist = -1.0;
        const int steps = (j - i + n) % n;
        for (int s = 1; s < steps; ++s) {
            const int pos = (i + s) % n;
            const double d = segDist(points[pos], points[i], points[j]);
            if (d > outDist) {
                outDist = d;
                outPos = pos;
            }
        }
    };

    // Top-down priority DP: always split the interval with the largest deviation
    // next, so the point budget is spent where it matters. This hits an exact
    // budget (classic tolerance-only DP cannot), while the tolerance early-out
    // collapses pixel staircases (deviation <= ~0.71 px) instead of eating the
    // budget on a diagonal edge.
    struct Interval {
        int i, j, pos;
        double dist;
    };
    struct Cmp {
        bool operator()(const Interval& a, const Interval& b) const
        {
            if (a.dist != b.dist)
                return a.dist < b.dist; // max-heap on deviation
            return a.pos > b.pos;       // deterministic tie-break: lower pos first
        }
    };
    std::priority_queue<Interval, std::vector<Interval>, Cmp> pq;
    std::vector<char> kept((size_t)n, 0);
    kept[(size_t)a0] = 1;
    kept[(size_t)a1] = 1;
    int keptCount = 2;
    auto pushInterval = [&](int i, int j) {
        int pos;
        double dist;
        farthest(i, j, pos, dist);
        if (pos >= 0)
            pq.push({i, j, pos, dist});
    };
    pushInterval(a0, a1);
    pushInterval(a1, a0);
    while (!pq.empty() && keptCount < maxPoints) {
        const Interval top = pq.top();
        // Tolerance early-out only once a real polygon exists: a 1-px hole's
        // corner deviation (~0.707) sits under a 1.0 tolerance, and breaking
        // at the two anchors alone returned a 2-point "polygon" that the
        // rasterizer silently skips — welding tiny holes shut (#142 review).
        if (top.dist <= (double)tolerance && keptCount >= 3)
            break; // every remaining deviation is within tolerance
        pq.pop();
        kept[(size_t)top.pos] = 1; // intervals only split, never move -> never stale
        ++keptCount;
        pushInterval(top.i, top.pos);
        pushInterval(top.pos, top.j);
    }

    std::vector<vec2> out;
    out.reserve((size_t)keptCount);
    for (int i = 0; i < n; ++i)
        if (kept[(size_t)i])
            out.push_back(points[i]); // ring order preserved -> ordered subset
    return out;
}

SilhouetteMask RasterizePolygons(const std::vector<SilhouetteContour>& contours, int width,
                                 int height)
{
    SilhouetteMask m = MakeMask(width, height);
    if (width <= 0 || height <= 0)
        return m;
    std::vector<double> xs;
    for (int y = 0; y < height; ++y) {
        const double yc = (double)y + 0.5;
        xs.clear();
        for (const SilhouetteContour& c : contours) {
            const std::vector<vec2>& p = c.points;
            const size_t np = p.size();
            if (np < 3)
                continue;
            for (size_t i = 0, j = np - 1; i < np; j = i++) {
                const double ay = p[i].y, by = p[j].y;
                // Half-open in y so a shared vertex counts once; a horizontal
                // edge (ay==by) never satisfies it and is skipped for free.
                // Vertices are integers and yc is half-integer, so the scanline
                // never passes through a vertex — no epsilon anywhere.
                if ((ay <= yc && yc < by) || (by <= yc && yc < ay)) {
                    const double t = (yc - ay) / (by - ay);
                    xs.push_back((double)p[i].x + t * ((double)p[j].x - (double)p[i].x));
                }
            }
        }
        if (xs.size() < 2)
            continue;
        std::sort(xs.begin(), xs.end());
        uint8_t* row = &m.pixels[(size_t)y * width];
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            // Pixel centers strictly inside (xL, xR): first x with x+0.5 > xL,
            // last with x+0.5 < xR. Double math, then clamp to the frame.
            int xStart = (int)std::floor(xs[k] - 0.5) + 1;
            int xEnd = (int)std::ceil(xs[k + 1] - 0.5) - 1;
            xStart = std::max(xStart, 0);
            xEnd = std::min(xEnd, width - 1);
            for (int x = xStart; x <= xEnd; ++x)
                row[x] = 255;
        }
    }
    return m;
}

SilhouetteLandmarks MaskLandmarks(const SilhouetteMask& mask)
{
    SilhouetteLandmarks lm;
    const int w = mask.width, h = mask.height;
    if (w <= 0 || h <= 0 || (size_t)w * h != mask.pixels.size())
        return lm;
    auto cov = [&](int x, int y) { return mask.pixels[(size_t)y * w + x] != 0; };

    int minX = w, minY = h, maxX = -1, maxY = -1;
    int64_t area = 0;
    double sumX = 0.0, sumY = 0.0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (cov(x, y)) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);
                ++area;
                sumX += (double)x + 0.5;
                sumY += (double)y + 0.5;
            }
    if (maxX < 0)
        return lm; // empty mask -> all -1 / 0

    lm.minX = minX;
    lm.minY = minY;
    lm.maxX = maxX;
    lm.maxY = maxY;
    lm.bboxWidth = maxX - minX + 1;
    lm.bboxHeight = maxY - minY + 1;
    lm.aspect = (float)lm.bboxWidth / (float)lm.bboxHeight;
    lm.area = (int)area;
    // int64 image area (#114 lesson): a 22.6 MP reference overflows int * 100.
    lm.areaFractionImage = (float)((double)area / ((double)w * (double)h));
    lm.fillFactor = (float)((double)area / ((double)lm.bboxWidth * (double)lm.bboxHeight));
    lm.centroid = vec2((float)(sumX / (double)area), (float)(sumY / (double)area));

    // Widest row / tallest column by covered SPAN (last - first + 1, gaps
    // included) — the extent, not the pixel count. First extreme wins ties.
    for (int y = minY; y <= maxY; ++y) {
        int l = -1, r = -1;
        for (int x = minX; x <= maxX; ++x)
            if (cov(x, y)) {
                if (l < 0)
                    l = x;
                r = x;
            }
        if (l >= 0 && r - l + 1 > lm.widestRowSpan) {
            lm.widestRowSpan = r - l + 1;
            lm.widestRowY = y;
            lm.widestRowLeft = l;
            lm.widestRowRight = r;
        }
    }
    for (int x = minX; x <= maxX; ++x) {
        int t = -1, b = -1;
        for (int y = minY; y <= maxY; ++y)
            if (cov(x, y)) {
                if (t < 0)
                    t = y;
                b = y;
            }
        if (t >= 0 && b - t + 1 > lm.tallestColSpan) {
            lm.tallestColSpan = b - t + 1;
            lm.tallestColX = x;
            lm.tallestColTop = t;
            lm.tallestColBottom = b;
        }
    }

    // Extremities: midpoint of the covered extent on each extreme row/column,
    // in center coords ((left+right+1)/2, y+0.5) and mirror for columns.
    auto rowMid = [&](int y) {
        int l = -1, r = -1;
        for (int x = 0; x < w; ++x)
            if (cov(x, y)) {
                if (l < 0)
                    l = x;
                r = x;
            }
        return vec2(((float)l + (float)r + 1.0f) * 0.5f, (float)y + 0.5f);
    };
    auto colMid = [&](int x) {
        int t = -1, b = -1;
        for (int y = 0; y < h; ++y)
            if (cov(x, y)) {
                if (t < 0)
                    t = y;
                b = y;
            }
        return vec2((float)x + 0.5f, ((float)t + (float)b + 1.0f) * 0.5f);
    };
    lm.top = rowMid(minY);
    lm.bottom = rowMid(maxY);
    lm.left = colMid(minX);
    lm.right = colMid(maxX);
    return lm;
}

std::vector<SilhouetteRowSpan> MaskRowSpans(const SilhouetteMask& mask, int rows)
{
    std::vector<SilhouetteRowSpan> spans;
    const int w = mask.width, h = mask.height;
    if (rows <= 0 || w <= 0 || h <= 0 || (size_t)w * h != mask.pixels.size())
        return spans;
    const SilhouetteLandmarks lm = MaskLandmarks(mask);
    if (lm.area == 0)
        return spans;
    const int bboxH = lm.bboxHeight;
    rows = std::min(rows, bboxH); // clamp so every sampled row is distinct

    auto cov = [&](int x, int y) { return mask.pixels[(size_t)y * w + x] != 0; };
    auto measure = [&](int y) {
        SilhouetteRowSpan s;
        s.y = y;
        for (int x = 0; x < w; ++x)
            if (cov(x, y)) {
                if (s.left < 0)
                    s.left = x;
                s.right = x;
                ++s.covered;
            }
        return s;
    };
    if (rows == 1) {
        spans.push_back(measure(lm.minY + (bboxH - 1) / 2)); // bbox middle row
        return spans;
    }
    // First row = minY, last = maxY, evenly spaced between (round to pixels).
    for (int i = 0; i < rows; ++i) {
        const int y =
            lm.minY + (int)std::lround((double)i * (double)(bboxH - 1) / (double)(rows - 1));
        spans.push_back(measure(y));
    }
    return spans;
}

std::vector<vec2> FoldOutline(const std::vector<vec2>& outline, float axisX)
{
    std::vector<vec2> out;
    const int n = (int)outline.size();
    if (n < 2)
        return out;
    // bottom = max y (tie max x); top = min y (tie max x).
    int bottom = 0, top = 0;
    for (int i = 1; i < n; ++i) {
        if (outline[i].y > outline[bottom].y ||
            (outline[i].y == outline[bottom].y && outline[i].x > outline[bottom].x))
            bottom = i;
        if (outline[i].y < outline[top].y ||
            (outline[i].y == outline[top].y && outline[i].x > outline[top].x))
            top = i;
    }
    if (bottom == top)
        return out; // no vertical extent to fold

    // Two walks bottom -> top around the ring; keep the side with the larger
    // mean x (the right half). Following the contour, not a row scan, preserves
    // a lip overhang's multi-valued r(y).
    auto walk = [&](int dir) {
        std::vector<vec2> path;
        int i = bottom;
        path.push_back(outline[(size_t)i]);
        while (i != top) {
            i = (i + dir + n) % n;
            path.push_back(outline[(size_t)i]);
        }
        return path;
    };
    const std::vector<vec2> fwd = walk(+1);
    const std::vector<vec2> bwd = walk(-1);
    auto meanX = [](const std::vector<vec2>& p) {
        double s = 0.0;
        for (const vec2& v : p)
            s += v.x;
        return p.empty() ? 0.0 : s / (double)p.size();
    };
    const std::vector<vec2>& side = meanX(fwd) >= meanX(bwd) ? fwd : bwd;
    out.reserve(side.size());
    for (const vec2& v : side)
        out.push_back(vec2(std::max(0.0f, v.x - axisX), v.y)); // r >= 0, bottom -> top
    return out;
}

} // namespace forge
