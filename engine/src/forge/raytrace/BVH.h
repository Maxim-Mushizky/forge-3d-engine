#pragma once

#include "forge/core/Geometry.h"

#include <vector>

namespace forge {

struct BVHTriangle {
    vec3 v0, v1, v2;
    vec3 n0, n1, n2;
    int material = 0;
    vec3 centroid;
};

// Node layout matches the GPU side: interior nodes store the left child index
// (right child is always left+1); leaves store the first triangle and a count.
struct BVHNode {
    vec3 min{0.0f};
    int leftFirst = 0;
    vec3 max{0.0f};
    int count = 0; // 0 = interior
};

class BVH {
public:
    // Median-split build. Reorders `tris` in place.
    void Build(std::vector<BVHTriangle>& tris);
    const std::vector<BVHNode>& Nodes() const { return m_Nodes; }

private:
    void FillNode(size_t nodeIndex, std::vector<BVHTriangle>& tris, int first, int count);

    std::vector<BVHNode> m_Nodes;
};

} // namespace forge
