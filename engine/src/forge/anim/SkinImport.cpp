#include "SkinImport.h"

#include "forge/core/Log.h"

namespace forge {

JointOrder TopoSortJoints(const std::vector<int>& parents)
{
    const int count = (int)parents.size();
    JointOrder result;
    result.order.reserve(count);
    result.remap.assign(count, -1);
    result.sortedParents.reserve(count);

    // Emit-when-parent-emitted, rescanning in input order: already parent-first
    // input therefore emits as the identity permutation (stability). O(n²)
    // worst case is fine — rigs are tens of joints, not thousands.
    std::vector<bool> emitted(count, false);
    int done = 0;
    while (done < count) {
        bool progress = false;
        for (int old = 0; old < count; ++old) {
            if (emitted[old])
                continue;
            const int parent = parents[old];
            const bool root = parent < 0 || parent >= count; // out-of-range parent = root
            if (!root && !emitted[parent])
                continue;
            result.remap[old] = (int)result.order.size();
            result.sortedParents.push_back(root ? -1 : result.remap[parent]);
            result.order.push_back(old);
            emitted[old] = true;
            ++done;
            progress = true;
        }
        if (!progress) {
            // Parent cycle (hostile or corrupt file): force the first stuck
            // joint to be a root and keep going — never hang on asset data.
            for (int old = 0; old < count; ++old) {
                if (emitted[old])
                    continue;
                FORGE_WARN("Skin import: joint parent cycle at joint %d — broken to root", old);
                result.remap[old] = (int)result.order.size();
                result.sortedParents.push_back(-1);
                result.order.push_back(old);
                emitted[old] = true;
                ++done;
                break;
            }
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
    const float sum = weights.x + weights.y + weights.z + weights.w;
    return sum > 1e-6f ? weights / sum : weights;
}

} // namespace forge
