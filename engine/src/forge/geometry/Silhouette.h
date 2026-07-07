#pragma once

#include "forge/core/Geometry.h"
#include "forge/core/Math.h"
#include "forge/renderer/Mesh.h" // Vertex

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace forge {

// GL-free silhouette kernel (#114): software-rasterize a mesh's orthographic
// outline into a binary mask, binarize reference images, normalize masks for
// shape comparison, and score overlap (IoU) — the numeric backbone of
// compare_silhouette, so "this looks like a real cup" becomes a measurement
// instead of an eyeball call. No GL anywhere: the whole pipeline runs in the
// unit tests.

struct SilhouetteMask {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels; // row-major, row 0 = top; 0 = empty, 255 = covered
};

SilhouetteMask MakeMask(int width, int height); // zeroed

int MaskArea(const SilhouetteMask& mask); // covered pixel count

// Orthographic view-projection fitted to `bounds` for a named view: "front"
// (looking down -z), "back", "left", "right", "top" (plan view, world +z down
// the image). Square frame sized to the larger in-plane extent so nothing
// clips; framing margin is irrelevant after NormalizeMask. nullopt for an
// unknown view name or invalid bounds.
std::optional<mat4> SilhouetteViewProj(const std::string& view, const AABB& bounds);

// OR the mesh's coverage into `mask` (accumulates across calls, so multi-mesh
// subtrees rasterize into one silhouette). mvp = viewProj * model. Vertices
// snap to a 1/16-subpixel integer grid and edges evaluate in int64 — exact
// coverage, no epsilon tuning, no shared-edge pinholes. Winding-independent:
// back-facing triangles count (a silhouette has no facing).
void RasterizeSilhouette(const std::vector<Vertex>& vertices,
                         const std::vector<uint32_t>& indices, const mat4& mvp,
                         SilhouetteMask& mask);

// Foreground mask from an RGBA8 reference image. If the alpha channel carries
// information (any pixel meaningfully transparent) it is the matte: alpha >=
// 128 is foreground. A strict two-valued gray image — every pixel opaque,
// r==g==b, at most two distinct values separated by >= 128 — is a silhouette
// PNG round-tripped as a reference (#135): the border-majority value is
// background, the other is figure, taken straight, because the dark-ground
// flood below would weld the axe's enclosed cutouts shut on the way back in
// (PunchEnclosedHoles bails on a dark median by design). A single-valued image
// yields an empty mask (caller's empty handling). JPEG chroma noise alone
// breaks the two-value test, so a photograph never reaches this rung. Opaque
// images with real tonal range segment by background flood from the
// border (tolerance from the border's median/MAD): enclosed regions the same
// color as the ground — white porcelain on a white product-shot ground — stay
// with the object, where a global threshold would split them wrong. On a
// NEUTRAL ground (border chroma spread near zero) the flood is additionally
// chroma-gated (#134): it never enters pixels tinted beyond the border's own
// spread, so bright warm material (edge-lit steel ramping seamlessly into a
// white sweep) doesn't get eaten. Enclosed through-holes (backdrop seen
// through an axe head's cutouts, unreachable by the flood) are then recovered
// on bright neutral grounds: fully enclosed, backdrop-neutral regions whose
// median luminance reads as SHADOWED GROUND (between half and four fifths of
// the border's) punch back to background unless doing so would orphan real
// figure. Colored grounds keep the pre-#134 flood exactly, with no recovery —
// the chroma guard that keeps tinted recesses safe has nothing to say there,
// so holes on colored grounds remain the alpha-matte case. Falls
// back to Otsu + border-majority polarity when the border is cluttered or the
// flood degenerates. Tiny connected components (compression specks, soft
// shadows) that would poison the crop box are dropped — under 1/20 of the
// largest goes; real secondary parts (a lid floating above a jar) survive. An
// alpha matte remains the explicit override when the automatic call is wrong.
SilhouetteMask BinarizeImage(const uint8_t* rgba, int width, int height);

// Translation/scale-invariant canonical form: tight-crop to the covered bbox,
// uniformly scale so the larger side fills outSize (aspect ratio preserved —
// a tall cup must NOT compare equal to a squat one), center-pad the rest.
// Empty input -> empty outSize x outSize mask.
SilhouetteMask NormalizeMask(const SilhouetteMask& in, int outSize);

struct SilhouetteDiff {
    float iou = 0.0f;     // intersection / union; 0 when either mask is empty
    float dice = 0.0f;    // 2*inter / (areaA + areaB)
    int intersection = 0; // pixels covered in both
    int unionArea = 0;
    int onlyA = 0; // covered in A only
    int onlyB = 0;
};

// Pixel-wise overlap of two same-size masks. Mismatched sizes score zero —
// callers normalize to a common resolution first.
SilhouetteDiff CompareMasks(const SilhouetteMask& a, const SilhouetteMask& b);

// RGBA8 visualization (row 0 = top): light gray = covered in both, green =
// only in A (the render), magenta = only in B (the reference) — green/magenta
// instead of red/green so the diff survives common color blindness. Empty on
// size mismatch.
std::vector<uint8_t> DiffImageRGBA(const SilhouetteMask& a, const SilhouetteMask& b);

// --- outline extraction (#135): mask -> simplified polygon + landmarks --------
// The ingest half of the shape loop (compare_silhouette is the iterate half):
// turn a reference mask into vector geometry the modeling tools can consume —
// a simplified outer contour with its holes, plus landmarks and a mirror fold
// for lathe/sweep profiles. All GL-free, integer-exact where it counts.

// Shoelace signed area of a closed polygon in the y-DOWN image frame: outer
// boundaries (traced with covered pixels on the interior side) come out
// POSITIVE, holes NEGATIVE. Double is exact for these magnitudes (corner
// coords <= 4097, so even a 50k-vertex sum stays well under 2^53).
double PolygonSignedArea(const std::vector<vec2>& points);
// Shoelace centroid; a degenerate (near-zero area) polygon falls back to the
// vertex mean so callers always get a finite point.
vec2 PolygonCentroid(const std::vector<vec2>& points);

// One traced boundary loop of a mask's covered region, on the pixel-CORNER
// lattice (x right, y DOWN — image space, row 0 top). Closed implicitly
// (points.back() connects to points.front()); vertices are integers stored in
// float vec2 (exact to 2^24).
struct SilhouetteContour {
    std::vector<vec2> points;
    double area = 0.0; // signed shoelace, px^2; > 0 = outer boundary, < 0 = hole
    bool hole = false; // area < 0
    int parent = -1;   // index of the immediately containing contour, -1 = top level
};

// Crack-follow the boundary of the union of covered unit pixel squares. Outer
// loops come back with positive area, holes negative; collinear runs are
// merged (a rect is 4 points); each loop's immediate parent is the smallest
// containing other loop by even-odd point test. Rasterizing the UNSIMPLIFIED
// result reproduces the source mask exactly (see RasterizePolygons). A
// nonzero minArea drops every loop with |area| < minArea (px^2) before parent
// assignment — a deliberate exactness-for-bounded-work trade: a pathological
// mask (grainy scan, checkerboard dither) peppers thousands of pinhole loops
// into the quadratic parent pass. The default stays 0.0 because the
// trace<->raster inverse is this kernel's contract; dropping detail is policy
// and belongs to callers. Empty or invalid mask -> empty vector.
std::vector<SilhouetteContour> TraceContours(const SilhouetteMask& mask,
                                             double minArea = 0.0);

// Simplify a CLOSED contour to at most maxPoints vertices, stopping earlier
// once the largest deviation of a dropped point falls to <= tolerance (px).
// Result is an ordered SUBSET of the input points, so integer inputs stay
// integer (keeping RasterizePolygons epsilon-free). maxPoints clamps to >= 3;
// input with < 3 points returns as-is.
std::vector<vec2> SimplifyContour(const std::vector<vec2>& points, int maxPoints,
                                  float tolerance);

// Even-odd scanline fill of closed polygons (outer contours and holes together
// — even-odd punches the holes for free) into a fresh mask, pixel centers
// sampled. Inverse of TraceContours: rasterizing an unsimplified trace
// reproduces the source mask EXACTLY.
SilhouetteMask RasterizePolygons(const std::vector<SilhouetteContour>& contours,
                                 int width, int height);

struct SilhouetteLandmarks {
    // Tight bbox of covered pixels, inclusive pixel indices; -1 all round when empty.
    int minX = -1, minY = -1, maxX = -1, maxY = -1;
    int bboxWidth = 0, bboxHeight = 0; // maxX-minX+1 etc.; 0 when empty
    float aspect = 0.0f;               // bboxWidth / bboxHeight
    int area = 0;                      // covered pixel count
    float areaFractionImage = 0.0f;    // area / (width*height)
    float fillFactor = 0.0f;           // area / (bboxWidth*bboxHeight)
    vec2 centroid{0.0f};               // mean of covered pixel CENTERS (x+0.5, y+0.5)
    // Widest row / tallest column by covered SPAN (right-left+1, not count):
    int widestRowY = -1, widestRowLeft = -1, widestRowRight = -1, widestRowSpan = 0;
    int tallestColX = -1, tallestColTop = -1, tallestColBottom = -1, tallestColSpan = 0;
    // Extremities: for the topmost covered row, the midpoint of its covered
    // extent ((left+right+1)/2 in corner coords, y+0.5) — likewise the others.
    vec2 top{0.0f}, bottom{0.0f}, left{0.0f}, right{0.0f};
};
SilhouetteLandmarks MaskLandmarks(const SilhouetteMask& mask);

// Per-row covered extent, sampled at `rows` evenly spaced rows across the bbox
// (first and last row included). The scanline measurement that cracks axe
// proportions. rows <= 0 or empty mask -> empty; rows clamps to bboxHeight so
// each sampled row is distinct.
struct SilhouetteRowSpan {
    int y = -1;                // pixel row
    int left = -1, right = -1; // covered extent, inclusive; -1/-1 = empty row
    int covered = 0;           // covered pixel count in the row (gaps excluded)
};
std::vector<SilhouetteRowSpan> MaskRowSpans(const SilhouetteMask& mask, int rows);

// Right-half chain of a closed outline about the vertical axis x = axisX: walk
// the contour from its bottom-most vertex to its top-most along the side with
// the larger mean x, emitting (r, y) with r = max(0, x - axisX). Bottom and
// top vertices are both included; output runs bottom (max y) -> top (min y) in
// IMAGE terms (the caller flips to y-up). Following the contour keeps a lip
// overhang's multi-valued r(y) that a per-row span table cannot represent. No
// axis points prepended/appended — the caller decides lathe closure.
std::vector<vec2> FoldOutline(const std::vector<vec2>& outline, float axisX);

} // namespace forge
