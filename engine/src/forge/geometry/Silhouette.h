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
// 128 is foreground. Opaque images segment by background flood from the
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

} // namespace forge
