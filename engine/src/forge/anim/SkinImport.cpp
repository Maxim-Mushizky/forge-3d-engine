#include "SkinImport.h"

#include "forge/core/Log.h"

#include <cmath>
#include <queue>

namespace forge {

JointOrder TopoSortJoints(const std::vector<int>& parents)
{
    const int count = (int)parents.size();
    JointOrder result;
    result.order.reserve(count);
    result.remap.assign(count, -1);
    result.sortedParents.reserve(count);

    // Smallest-ready-index-first (min-heap Kahn): already parent-first input
    // emits as the identity permutation (stability — the smallest unemitted
    // index always has its parent emitted, so it is always the heap top), and
    // the joint count is file-supplied, so O(n log n) matters: a rescan-per-
    // emission O(n²) sort was a freeze vector on hostile 100k-joint files.
    std::vector<std::vector<int>> children(count);
    std::vector<bool> emitted(count, false);
    std::priority_queue<int, std::vector<int>, std::greater<int>> ready;
    for (int i = 0; i < count; ++i) {
        const int parent = parents[i];
        // Out-of-range or self parent = root.
        if (parent >= 0 && parent < count && parent != i)
            children[parent].push_back(i);
        else
            ready.push(i);
    }

    auto emit = [&](int old, bool forcedRoot) {
        result.remap[old] = (int)result.order.size();
        const int parent = parents[old];
        const bool root = forcedRoot || parent < 0 || parent >= count || parent == old;
        result.sortedParents.push_back(root ? -1 : result.remap[parent]);
        result.order.push_back(old);
        emitted[old] = true;
        for (int child : children[old])
            ready.push(child);
    };

    int done = 0, cycleScan = 0;
    while (done < count) {
        if (!ready.empty()) {
            const int old = ready.top();
            ready.pop();
            if (emitted[old])
                continue; // stale re-push: a broken cycle's parent emitted after its child
            emit(old, false);
            ++done;
        } else {
            // Parent cycle (hostile or corrupt file): force the first stuck
            // joint to be a root and keep going — never hang on asset data.
            while (cycleScan < count && emitted[cycleScan])
                ++cycleScan;
            FORGE_WARN("Skin import: joint parent cycle at joint %d — broken to root", cycleScan);
            emit(cycleScan, true);
            ++done;
        }
    }
    return result;
}

void RemapVertexJoints(std::vector<VertexSkin>& skin, const std::vector<int>& remap)
{
    for (VertexSkin& vs : skin)
        for (int c = 0; c < 4; ++c) {
            const uint32_t j = vs.joints[c];
            vs.joints[c] = j < remap.size() ? (uint32_t)remap[j] : 0u;
        }
}

glm::uvec4 DecodeJointIndices(const uint8_t* data, int componentType)
{
    switch (componentType) {
    case kGltfUnsignedByte:
        return glm::uvec4(data[0], data[1], data[2], data[3]);
    case kGltfUnsignedShort: {
        const uint16_t* v = (const uint16_t*)data;
        return glm::uvec4(v[0], v[1], v[2], v[3]);
    }
    case kGltfUnsignedInt: { // spec-illegal for JOINTS_0; decoded defensively
        const uint32_t* v = (const uint32_t*)data;
        return glm::uvec4(v[0], v[1], v[2], v[3]);
    }
    default:
        return glm::uvec4(0);
    }
}

vec4 DecodeWeights(const uint8_t* data, int componentType)
{
    switch (componentType) {
    case kGltfFloat: {
        const float* v = (const float*)data;
        return vec4(v[0], v[1], v[2], v[3]);
    }
    case kGltfUnsignedByte:
        return vec4(data[0], data[1], data[2], data[3]) / 255.0f;
    case kGltfUnsignedShort: {
        const uint16_t* v = (const uint16_t*)data;
        return vec4(v[0], v[1], v[2], v[3]) / 65535.0f;
    }
    default:
        return vec4(0.0f);
    }
}

vec4 RenormalizeWeights(const vec4& weights)
{
    // NaN/Inf/negative components would poison the LBS sum — the kernel drops
    // them via !(w > 0) and then scales positions by the PARTIAL weight sum,
    // a spike artifact instead of a clean passthrough. Zero them first.
    // !(w >= 0) is deliberate: it also catches NaN.
    vec4 w = weights;
    for (int c = 0; c < 4; ++c)
        if (!(w[c] >= 0.0f) || !std::isfinite(w[c]))
            w[c] = 0.0f;
    const float sum = w.x + w.y + w.z + w.w;
    // Sub-epsilon sums collapse to a literal zero, not the tiny original: the
    // skinning kernel treats weightSum > 0 as skinnable and would scale the
    // vertex by ~1e-8 (a spike to the origin) instead of leaving it at bind.
    return sum > 1e-6f ? w / sum : vec4(0.0f);
}

} // namespace forge
