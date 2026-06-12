#include "BVH.h"

#include <algorithm>

namespace forge {

static constexpr int kLeafSize = 4;

void BVH::Build(std::vector<BVHTriangle>& tris)
{
    m_Nodes.clear();
    if (tris.empty())
        return;
    m_Nodes.reserve(tris.size() * 2);
    m_Nodes.emplace_back();
    FillNode(0, tris, 0, (int)tris.size());
}

void BVH::FillNode(size_t nodeIndex, std::vector<BVHTriangle>& tris, int first, int count)
{
    AABB bounds, centroidBounds;
    for (int i = first; i < first + count; ++i) {
        bounds.Expand(tris[i].v0);
        bounds.Expand(tris[i].v1);
        bounds.Expand(tris[i].v2);
        centroidBounds.Expand(tris[i].centroid);
    }

    vec3 extent = centroidBounds.max - centroidBounds.min;
    int axis = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2) : (extent.y > extent.z ? 1 : 2);

    if (count <= kLeafSize || extent[axis] < 1e-8f) {
        m_Nodes[nodeIndex] = BVHNode{bounds.min, first, bounds.max, count};
        return;
    }

    int half = count / 2;
    std::nth_element(tris.begin() + first, tris.begin() + first + half, tris.begin() + first + count,
                     [axis](const BVHTriangle& a, const BVHTriangle& b) { return a.centroid[axis] < b.centroid[axis]; });

    // Children are allocated adjacently so the GPU can address them as left, left+1.
    int left = (int)m_Nodes.size();
    m_Nodes.emplace_back();
    m_Nodes.emplace_back();
    m_Nodes[nodeIndex] = BVHNode{bounds.min, left, bounds.max, 0};

    FillNode((size_t)left, tris, first, half);
    FillNode((size_t)left + 1, tris, first + half, count - half);
}

} // namespace forge
