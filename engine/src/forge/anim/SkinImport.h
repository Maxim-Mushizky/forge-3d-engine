#pragma once

#include "forge/core/Math.h"
#include "forge/renderer/Mesh.h" // VertexSkin — POD only, no GL

#include <cstdint>
#include <vector>

namespace forge {

// Pure glTF-skin parse helpers, free of tinygltf types so the parse logic
// unit-tests headless. ModelImporter.cpp adapts accessor data into these.

// glTF accessor component types (numerically the GL enums — spelled out here
// so this header stays tinygltf-free).
constexpr int kGltfUnsignedByte = 5121;
constexpr int kGltfUnsignedShort = 5123;
constexpr int kGltfUnsignedInt = 5125;
constexpr int kGltfFloat = 5126;

// Topological order for a joint forest so parents[i] < i holds afterwards.
// glTF does NOT guarantee parent-first joint arrays, so sorting is mandatory,
// not defensive.
struct JointOrder {
    std::vector<int> order;         // new index -> old index (feed to ReorderJoints)
    std::vector<int> remap;         // old index -> new index (feed to RemapVertexJoints)
    std::vector<int> sortedParents; // new-indexed, -1 = root, always < own index
};

// Stable: input that is already parent-first comes back as the identity
// permutation. Out-of-range parents become roots; parent cycles are broken
// into roots (warned) so a hostile file cannot hang the sort. The result is
// always a valid permutation.
JointOrder TopoSortJoints(const std::vector<int>& parents);

// out[newIdx] = values[order[newIdx]]. Apply to EVERY per-joint attribute
// (names, TRS, IBMs) — reordering some but not all desynchronizes the skeleton
// (the classic forgotten-IBM bug; unit-tested). Size mismatch returns the
// input unchanged rather than reading out of bounds.
template <typename T>
std::vector<T> ReorderJoints(const std::vector<T>& values, const std::vector<int>& order)
{
    if (values.size() != order.size())
        return values;
    std::vector<T> out;
    out.reserve(order.size());
    for (int oldIdx : order)
        out.push_back(values[(size_t)oldIdx]);
    return out;
}

// Rewrites per-vertex joint indices old -> new. Indices outside the remap are
// zeroed — the importer has already zeroed their weights.
void RemapVertexJoints(std::vector<VertexSkin>& skin, const std::vector<int>& remap);

// One VEC4 element at data. JOINTS_0 is u8/u16 per spec (+u32 defensively);
// WEIGHTS_0 is float or normalized u8 (/255) / u16 (/65535). Unknown component
// types decode to zero, which downstream treats as an unweighted vertex.
glm::uvec4 DecodeJointIndices(const uint8_t* data, int componentType);
vec4 DecodeWeights(const uint8_t* data, int componentType);

// Zeroes NaN/Inf/negative components (external data — a poisoned component
// would scale skinned positions by the partial weight sum), then w /= sum when
// sum > epsilon; a near-zero sum returns all-zero so the skinning kernel passes
// that vertex through at bind pose (a tiny nonzero sum would spike it instead).
vec4 RenormalizeWeights(const vec4& weights);

} // namespace forge
