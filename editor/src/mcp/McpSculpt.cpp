#include "McpSculpt.h"

#include <glm/glm.hpp>

#include <utility>

namespace forge {

float FalloffWeight(SculptFalloff falloff, float t)
{
    if (t >= 1.0f)
        return 0.0f;
    t = glm::max(t, 0.0f);
    switch (falloff) {
    case SculptFalloff::Linear:
        return 1.0f - t;
    case SculptFalloff::Constant:
        return 1.0f;
    case SculptFalloff::Smooth:
    default:
        return 1.0f - (3.0f * t * t - 2.0f * t * t * t); // matches SculptTool::Falloff
    }
}

namespace {

struct Affected {
    uint32_t group;
    float weight;
};

// Weld groups whose representative (group[0], same convention as SculptTool)
// falls inside the brush. Weights <= 0.001 are culled like the interactive
// brushes; groups pointing past the vertex array (stale topology) are skipped
// rather than trusted.
std::vector<Affected> GatherRegion(const std::vector<Vertex>& verts, const MeshTopology& topo,
                                   const vec3& center, float radius, SculptFalloff falloff)
{
    std::vector<Affected> region;
    if (!(radius > 0.0f))
        return region;
    for (size_t g = 0; g < topo.groups.size(); ++g) {
        const auto& group = topo.groups[g];
        if (group.empty() || group[0] >= verts.size())
            continue;
        const vec3& rep = verts[group[0]].position;
        const float w = FalloffWeight(falloff, glm::length(rep - center) / radius);
        if (w > 0.001f)
            region.push_back({(uint32_t)g, w});
    }
    return region;
}

void WriteGroup(std::vector<Vertex>& verts, const MeshTopology& topo, uint32_t g, const vec3& pos)
{
    for (uint32_t i : topo.groups[g])
        if (i < verts.size())
            verts[i].position = pos;
}

} // namespace

size_t SculptMove(std::vector<Vertex>& verts, const MeshTopology& topo, const vec3& center,
                  float radius, const vec3& offset, SculptFalloff falloff)
{
    const std::vector<Affected> region = GatherRegion(verts, topo, center, radius, falloff);
    for (const Affected& a : region) {
        const vec3 rep = verts[topo.groups[a.group][0]].position;
        WriteGroup(verts, topo, a.group, rep + offset * a.weight);
    }
    return region.size();
}

size_t SculptInflate(std::vector<Vertex>& verts, const MeshTopology& topo, const vec3& center,
                     float radius, float amount)
{
    const std::vector<Affected> region = GatherRegion(verts, topo, center, radius,
                                                      SculptFalloff::Smooth);
    for (const Affected& a : region) {
        const Vertex& rep = verts[topo.groups[a.group][0]];
        WriteGroup(verts, topo, a.group, rep.position + rep.normal * (amount * a.weight));
    }
    return region.size();
}

size_t SculptSmooth(std::vector<Vertex>& verts, const MeshTopology& topo, const vec3& center,
                    float radius, float strength)
{
    const std::vector<Affected> region = GatherRegion(verts, topo, center, radius,
                                                      SculptFalloff::Smooth);
    // Two-phase: every target comes from pre-move positions. An in-place
    // Laplacian would let groups processed earlier drag their neighbors'
    // averages — the result would depend on group order.
    std::vector<std::pair<uint32_t, vec3>> moves;
    moves.reserve(region.size());
    for (const Affected& a : region) {
        if (a.group >= topo.groupNeighbors.size())
            continue;
        const auto& neighbors = topo.groupNeighbors[a.group];
        vec3 avg(0.0f);
        size_t count = 0;
        for (uint32_t ng : neighbors) {
            if (ng >= topo.groups.size() || topo.groups[ng].empty() ||
                topo.groups[ng][0] >= verts.size())
                continue;
            avg += verts[topo.groups[ng][0]].position;
            ++count;
        }
        if (count == 0)
            continue; // isolated vertex: nothing to relax toward
        avg /= (float)count;
        const vec3 rep = verts[topo.groups[a.group][0]].position;
        moves.push_back({a.group, glm::mix(rep, avg, glm::clamp(a.weight * strength, 0.0f, 1.0f))});
    }
    for (const auto& [g, pos] : moves)
        WriteGroup(verts, topo, g, pos);
    return moves.size();
}

} // namespace forge
