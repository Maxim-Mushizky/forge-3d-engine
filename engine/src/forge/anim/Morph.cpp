#include "Morph.h"

#include "forge/core/Log.h"

namespace forge {

void MorphVertices(const std::vector<Vertex>& bind, const std::vector<MorphTarget>& targets,
                   const std::vector<float>& weights, std::vector<Vertex>& out)
{
    out = bind; // passthrough baseline: zero/empty weights leave the bind data bit-exact
    const size_t count = targets.size() < weights.size() ? targets.size() : weights.size();
    for (size_t t = 0; t < count; ++t) {
        const float w = weights[t];
        if (w == 0.0f)
            continue; // 52-target face rigs usually drive a handful at a time
        const MorphTarget& target = targets[t];
        if (target.positionDeltas.size() != bind.size()) {
            // Reachable from external asset data — skip the target, never assert.
            FORGE_WARN("MorphVertices: target \"%s\" has %zu deltas for %zu vertices — skipped",
                       target.name.c_str(), target.positionDeltas.size(), bind.size());
            continue;
        }
        for (size_t v = 0; v < bind.size(); ++v)
            out[v].position += w * target.positionDeltas[v];
        if (target.normalDeltas.size() == bind.size())
            for (size_t v = 0; v < bind.size(); ++v)
                out[v].normal += w * target.normalDeltas[v];
    }
}

void TransformMorphDeltas(std::vector<MorphTarget>& targets, const mat3& positionXf,
                          const mat3& normalXf)
{
    for (MorphTarget& target : targets) {
        for (vec3& d : target.positionDeltas)
            d = positionXf * d;
        for (vec3& d : target.normalDeltas)
            d = normalXf * d;
    }
}

bool ApplySparseOverlay(std::vector<vec3>& base, const uint8_t* indicesData,
                        int indicesComponentType, const uint8_t* valuesData, size_t sparseCount)
{
    if (!indicesData || !valuesData)
        return false;
    const vec3* values = (const vec3*)valuesData; // VEC3 float, tightly packed per spec
    uint64_t previous = 0;
    for (size_t k = 0; k < sparseCount; ++k) {
        uint64_t index;
        switch (indicesComponentType) {
        case kGltfUnsignedByte: index = indicesData[k]; break;
        case kGltfUnsignedShort: index = ((const uint16_t*)indicesData)[k]; break;
        case kGltfUnsignedInt: index = ((const uint32_t*)indicesData)[k]; break;
        default:
            return false; // spec: u8/u16/u32 only (5124 SIGNED int is explicitly illegal)
        }
        if (index >= base.size())
            return false;
        if (k > 0 && index <= previous)
            return false; // spec: strictly increasing — a violation flags a corrupt file
        base[index] = values[k];
        previous = index;
    }
    return true;
}

} // namespace forge
