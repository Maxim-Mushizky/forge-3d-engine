#include "Skinning.h"

#include "forge/core/Log.h"

namespace forge {

void SkinVertices(const std::vector<Vertex>& bind, const std::vector<VertexSkin>& skin,
                  const std::vector<mat4>& palette, std::vector<Vertex>& out)
{
    out = bind; // passthrough baseline: UVs always copy, unweighted vertices stay at bind
    if (skin.size() != bind.size()) {
        // Reachable from external asset data — degrade to the bind copy, never assert.
        FORGE_WARN("SkinVertices: %zu skin entries for %zu vertices — passthrough", skin.size(),
                   bind.size());
        return;
    }

    // Per-joint normal matrices once per call, not per influence.
    std::vector<mat3> normalMats(palette.size());
    for (size_t j = 0; j < palette.size(); ++j)
        normalMats[j] = glm::transpose(glm::inverse(mat3(palette[j])));

    for (size_t v = 0; v < bind.size(); ++v) {
        const VertexSkin& vs = skin[v];
        mat4 blend(0.0f);
        mat3 normalBlend(0.0f);
        float weightSum = 0.0f;
        for (int i = 0; i < 4; ++i) {
            const float w = vs.weights[i];
            const uint32_t j = vs.joints[i];
            // !(w > 0) also drops NaN/negative garbage; an out-of-range joint
            // index loses that one influence, never the whole vertex.
            if (!(w > 0.0f) || j >= palette.size())
                continue;
            blend += palette[j] * w;
            normalBlend += normalMats[j] * w;
            weightSum += w;
        }
        if (weightSum <= 0.0f) {
            // Unweighted vertex keeps the source position/normal — but normalize
            // the normal: the source may be a MORPHED bind carrying a raw delta
            // sum (#149), and this path would otherwise ship it to the VBO
            // unnormalized. Unit bind normals make this a no-op on skin-only
            // meshes; a degenerate sum keeps the source value rather than
            // dividing toward NaN (same guard as the weighted path below).
            const float len = glm::length(out[v].normal);
            if (len > 1e-8f)
                out[v].normal /= len;
            continue;
        }

        out[v].position = vec3(blend * vec4(bind[v].position, 1.0f));
        vec3 n = normalBlend * bind[v].normal;
        float len = glm::length(n);
        // Degenerate blend (opposing rotations, singular scale): keep the bind
        // normal rather than pushing NaN into the VBO.
        out[v].normal = len > 1e-8f ? n / len : bind[v].normal;
    }
}

} // namespace forge
