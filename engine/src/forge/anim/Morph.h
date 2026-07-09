#pragma once

#include "forge/anim/SkinImport.h" // kGltf component-type constants (tinygltf-free)
#include "forge/core/Math.h"
#include "forge/renderer/Mesh.h" // Vertex + MorphTarget PODs only — Mesh itself is never touched

#include <cstdint>
#include <vector>

namespace forge {

// Morph-target evaluation over plain vectors, GL-free so it unit-tests headless
// (same shape as SkinVertices). out = bind + Σ wᵢ·δᵢ for positions, and for
// normals of targets that carry normal deltas. The pure spec sum, deliberately:
// weights are NOT clamped (glTF allows negative and >1 — clamping is UI policy,
// not an engine invariant) and normals are NOT renormalized here — the final
// consumer normalizes once, after skinning (glTF normative order is morph THEN
// skin, and the scalar factors out). weights may be shorter than targets
// (missing = 0) or longer (extras ignored); zero-weight targets are skipped.
// A target whose delta arrays don't parallel `bind` is skipped with a warning.
void MorphVertices(const std::vector<Vertex>& bind, const std::vector<MorphTarget>& targets,
                   const std::vector<float>& weights, std::vector<Vertex>& out);

// Bakes a node transform into morph deltas (#149 review). Rigid glTF nodes bake
// their world transform into the base vertices at import, so the deltas must
// ride the same transform or morphs displace in raw model space under a
// world-baked mesh: position deltas by the linear part (directions — no
// translation), normal deltas by the inverse-transpose, exactly like the base
// attributes. Deltas stay raw (no normalize — they are summands, and the final
// consumer normalizes once after the sum). Skinned meshes never need this
// (their import world is forced identity).
void TransformMorphDeltas(std::vector<MorphTarget>& targets, const mat3& positionXf,
                          const mat3& normalXf);

// glTF sparse-accessor overlay (#149): base[indices[k]] = values[k] — a full
// element REPLACE, not an add. The caller supplies `base` already dense-read
// (or zero-filled when the accessor has no bufferView, the common encoding for
// face shapes) and raw pointers into the indices/values buffer views, bounds-
// checked for sparseCount elements. Pure and tinygltf-free for headless tests.
// Returns false (base half-written, caller must discard) on a spec violation:
// index component type outside u8/u16/u32 (5124 signed int is illegal), an
// out-of-range index, or indices that are not strictly increasing.
bool ApplySparseOverlay(std::vector<vec3>& base, const uint8_t* indicesData,
                        int indicesComponentType, const uint8_t* valuesData, size_t sparseCount);

} // namespace forge
