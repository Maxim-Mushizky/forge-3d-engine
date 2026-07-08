#include "test_framework.h"

#include <forge/anim/Skeleton.h>
#include <forge/anim/SkinImport.h>
#include <forge/anim/Skinning.h>

#include <limits>
#include <string>
#include <vector>

namespace forge::test {

namespace {

constexpr float kHalfPi = 1.57079632679f;

bool ApproxVec3(const vec3& a, const vec3& b, float eps = 1e-4f)
{
    return ApproxEq(a.x, b.x, eps) && ApproxEq(a.y, b.y, eps) && ApproxEq(a.z, b.z, eps);
}

bool ApproxMat4(const mat4& a, const mat4& b, float eps = 1e-4f)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!ApproxEq(a[c][r], b[c][r], eps))
                return false;
    return true;
}

Vertex MakeVert(const vec3& p, const vec3& n, const vec2& uv = {0.0f, 0.0f})
{
    Vertex v{};
    v.position = p;
    v.normal = n;
    v.uv = uv;
    return v;
}

VertexSkin MakeSkin(const glm::uvec4& joints, const vec4& weights)
{
    VertexSkin s;
    s.joints = joints;
    s.weights = weights;
    return s;
}

// Three joints stacked +1 Y apart, identity rotations, unit scale, with exact
// inverse-of-bind-global IBMs.
Skeleton MakeChain()
{
    Skeleton sk;
    sk.parents = {-1, 0, 1};
    sk.names = {"root", "mid", "tip"};
    sk.bindT = {vec3(0.0f), vec3(0, 1, 0), vec3(0, 1, 0)};
    sk.bindR.assign(3, quat(1.0f, 0.0f, 0.0f, 0.0f));
    sk.bindS.assign(3, vec3(1.0f));
    for (const mat4& g : ComputeBindGlobals(sk))
        sk.inverseBind.push_back(glm::inverse(g));
    return sk;
}

// --- 1. palette on a known 3-joint chain ------------------------------------
void TestPaletteChain()
{
    Skeleton sk = MakeChain();
    std::vector<mat4> globals = ComputeBindGlobals(sk);
    CHECK(globals.size() == 3);
    // Hand-computed: pure translation accumulation up the chain.
    CHECK(ApproxMat4(globals[0], mat4(1.0f)));
    CHECK(ApproxMat4(globals[1], glm::translate(mat4(1.0f), vec3(0, 1, 0))));
    CHECK(ApproxMat4(globals[2], glm::translate(mat4(1.0f), vec3(0, 2, 0))));

    // 90° Z on the middle joint swings the tip from +Y to -X (hand-computed):
    // globals must compose child-under-parent, not just stack locals.
    Skeleton bent = MakeChain();
    bent.bindR[1] = glm::angleAxis(kHalfPi, vec3(0, 0, 1));
    std::vector<mat4> bentGlobals = ComputeBindGlobals(bent);
    CHECK(ApproxVec3(vec3(bentGlobals[2][3]), vec3(-1, 1, 0)));

    // inverseBind = inverse(bindGlobal) -> palette collapses to identity: the
    // bind-pose identity property R1's rendering rests on.
    std::vector<mat4> palette = ComputePalette(globals, sk.inverseBind);
    CHECK(palette.size() == 3);
    for (const mat4& m : palette)
        CHECK(ApproxMat4(m, mat4(1.0f)));
}

// --- 2. bind-pose identity end-to-end ---------------------------------------
void TestBindPoseIdentity()
{
    Skeleton sk = MakeChain();
    std::vector<mat4> palette = ComputePalette(ComputeBindGlobals(sk), sk.inverseBind);

    std::vector<Vertex> bind = {
        MakeVert({0.3f, 0.1f, -0.2f}, {0, 1, 0}, {0.25f, 0.75f}),
        MakeVert({-0.1f, 1.4f, 0.6f}, glm::normalize(vec3(1, 1, 0)), {0.5f, 0.5f}),
        MakeVert({0.0f, 2.2f, 0.0f}, {0, 0, 1}, {1.0f, 0.0f}),
    };
    std::vector<VertexSkin> skin = {
        MakeSkin({0, 1, 0, 0}, {0.7f, 0.3f, 0, 0}),
        MakeSkin({0, 1, 2, 0}, {0.2f, 0.5f, 0.3f, 0}),
        MakeSkin({2, 0, 0, 0}, {1.0f, 0, 0, 0}),
    };
    std::vector<Vertex> out;
    SkinVertices(bind, skin, palette, out);
    CHECK(out.size() == bind.size());
    for (size_t i = 0; i < bind.size(); ++i) {
        CHECK(ApproxVec3(out[i].position, bind[i].position));
        CHECK(ApproxVec3(out[i].normal, bind[i].normal));
        CHECK(ApproxEq(out[i].uv.x, bind[i].uv.x) && ApproxEq(out[i].uv.y, bind[i].uv.y));
    }
}

// --- 3. blend correctness ----------------------------------------------------
void TestBlend()
{
    // Two root joints, joint 1 translated (+2,0,0), identity IBMs: the palette
    // reads directly as the joint globals.
    Skeleton sk;
    sk.parents = {-1, -1};
    sk.names = {"a", "b"};
    sk.bindT = {vec3(0.0f), vec3(2, 0, 0)};
    sk.bindR.assign(2, quat(1.0f, 0.0f, 0.0f, 0.0f));
    sk.bindS.assign(2, vec3(1.0f));
    sk.inverseBind.assign(2, mat4(1.0f));
    std::vector<mat4> palette = ComputePalette(ComputeBindGlobals(sk), sk.inverseBind);

    std::vector<Vertex> bind = {MakeVert({0, 0, 0}, {0, 1, 0})};
    std::vector<Vertex> out;

    // 50/50: halfway.
    std::vector<VertexSkin> skin = {MakeSkin({0, 1, 0, 0}, {0.5f, 0.5f, 0, 0})};
    SkinVertices(bind, skin, palette, out);
    CHECK(ApproxVec3(out[0].position, vec3(1, 0, 0)));

    // 100/0: the full offset.
    skin[0] = MakeSkin({1, 0, 0, 0}, {1.0f, 0, 0, 0});
    SkinVertices(bind, skin, palette, out);
    CHECK(ApproxVec3(out[0].position, vec3(2, 0, 0)));

    // Zero weight sum: passthrough at bind, position and normal.
    bind[0] = MakeVert({0.4f, -0.3f, 0.9f}, glm::normalize(vec3(1, 2, 3)));
    skin[0] = MakeSkin({0, 1, 0, 0}, vec4(0.0f));
    SkinVertices(bind, skin, palette, out);
    CHECK(ApproxVec3(out[0].position, bind[0].position));
    CHECK(ApproxVec3(out[0].normal, bind[0].normal));
}

// --- 4. rotated joint rotates normals ---------------------------------------
void TestRotatedNormals()
{
    // One joint rotated 90° about Z at full weight, identity IBM: geometry AND
    // normals must rotate with it.
    Skeleton sk;
    sk.parents = {-1};
    sk.names = {"j"};
    sk.bindT = {vec3(0.0f)};
    sk.bindR = {glm::angleAxis(kHalfPi, vec3(0, 0, 1))};
    sk.bindS = {vec3(1.0f)};
    sk.inverseBind = {mat4(1.0f)};
    std::vector<mat4> palette = ComputePalette(ComputeBindGlobals(sk), sk.inverseBind);

    std::vector<Vertex> bind = {MakeVert({1, 0, 0}, {1, 0, 0})};
    std::vector<VertexSkin> skin = {MakeSkin({0, 0, 0, 0}, {1, 0, 0, 0})};
    std::vector<Vertex> out;
    SkinVertices(bind, skin, palette, out);
    CHECK(ApproxVec3(out[0].position, vec3(0, 1, 0)));
    CHECK(ApproxVec3(out[0].normal, vec3(0, 1, 0)));
}

// --- 5. non-uniform scale normal regression ----------------------------------
void TestNonUniformScaleNormals()
{
    // Joint scale (2,1,1). The correct normal transform is the PER-JOINT
    // inverse transpose; naive mat3(palette) is provably different here.
    std::vector<mat4> palette = {glm::scale(mat4(1.0f), vec3(2, 1, 1))};
    vec3 n = glm::normalize(vec3(1, 0, 1)); // slanted plane normal
    std::vector<Vertex> bind = {MakeVert({1, 1, 1}, n)};
    std::vector<VertexSkin> skin = {MakeSkin({0, 0, 0, 0}, {1, 0, 0, 0})};
    std::vector<Vertex> out;
    SkinVertices(bind, skin, palette, out);

    CHECK(ApproxVec3(out[0].position, vec3(2, 1, 1)));

    vec3 correct = glm::normalize(vec3(0.5f * n.x, n.y, n.z)); // S^-T · n
    vec3 naive = glm::normalize(vec3(2.0f * n.x, n.y, n.z));   // mat3(palette) · n
    CHECK(ApproxVec3(out[0].normal, correct));
    CHECK(!ApproxVec3(out[0].normal, naive));
    CHECK(ApproxEq(glm::length(out[0].normal), 1.0f));
    // Still perpendicular to the deformed surface: a tangent of the original
    // plane pushed through the palette must stay orthogonal to the new normal.
    vec3 tangent = vec3(palette[0] * vec4(1, 0, -1, 0));
    CHECK(ApproxEq(glm::dot(out[0].normal, tangent), 0.0f));
}

// --- 6. topo sort + consistent remap ----------------------------------------
void TestTopoSortRemap()
{
    // Child-before-parent input (old order {tip, mid, root}): invalid for the
    // one-pass palette until sorted. glTF permits this ordering.
    std::vector<int> parents = {1, 2, -1};
    JointOrder jo = TopoSortJoints(parents);
    CHECK(jo.order.size() == 3 && jo.remap.size() == 3 && jo.sortedParents.size() == 3);
    for (size_t i = 0; i < jo.sortedParents.size(); ++i)
        CHECK(jo.sortedParents[i] < (int)i); // the Skeleton invariant
    CHECK((jo.order == std::vector<int>{2, 1, 0}));
    CHECK((jo.remap == std::vector<int>{2, 1, 0}));
    CHECK((jo.sortedParents == std::vector<int>{-1, 0, 1}));

    // Distinct sentinels per joint prove EVERY attribute moved together.
    std::vector<std::string> names = {"tip", "mid", "root"};
    std::vector<vec3> bindT = {vec3(3, 0, 0), vec3(2, 0, 0), vec3(1, 0, 0)};
    std::vector<mat4> ibms = {glm::translate(mat4(1.0f), vec3(30, 0, 0)),
                              glm::translate(mat4(1.0f), vec3(20, 0, 0)),
                              glm::translate(mat4(1.0f), vec3(10, 0, 0))};
    std::vector<std::string> sortedNames = ReorderJoints(names, jo.order);
    std::vector<vec3> sortedT = ReorderJoints(bindT, jo.order);
    std::vector<mat4> sortedIbms = ReorderJoints(ibms, jo.order);
    CHECK(sortedNames[0] == "root" && sortedNames[1] == "mid" && sortedNames[2] == "tip");
    CHECK(ApproxVec3(sortedT[0], vec3(1, 0, 0)) && ApproxVec3(sortedT[2], vec3(3, 0, 0)));
    CHECK(ApproxMat4(sortedIbms[0], ibms[2])); // the forgotten-IBM-reorder classic
    CHECK(ApproxMat4(sortedIbms[2], ibms[0]));

    // Vertex joint indices follow the same remap: each vertex still points at
    // the joint carrying its sentinel (old 0 "tip" -> new 2).
    std::vector<VertexSkin> skin = {MakeSkin({0, 1, 2, 0}, {0.5f, 0.3f, 0.2f, 0})};
    RemapVertexJoints(skin, jo.remap);
    CHECK(skin[0].joints == glm::uvec4(2, 1, 0, 2));

    // Already-sorted input is stable: identity permutation, parents unchanged.
    JointOrder id = TopoSortJoints({-1, 0, 0, 2});
    CHECK((id.order == std::vector<int>{0, 1, 2, 3}));
    CHECK((id.sortedParents == std::vector<int>{-1, 0, 0, 2}));

    // A parent cycle (hostile file) must terminate with a valid permutation.
    JointOrder cyc = TopoSortJoints({1, 0});
    CHECK(cyc.order.size() == 2);
    for (size_t i = 0; i < cyc.sortedParents.size(); ++i)
        CHECK(cyc.sortedParents[i] < (int)i);

    // Cycle plus downstream chain: the stale re-push from the break must not
    // emit a joint twice (permutation stays exact-size and valid).
    JointOrder cycChain = TopoSortJoints({1, 0, 0, 1});
    CHECK(cycChain.order.size() == 4 && cycChain.sortedParents.size() == 4);
    std::vector<bool> seen(4, false);
    for (int old : cycChain.order) {
        CHECK(old >= 0 && old < 4 && !seen[old]);
        seen[old] = true;
    }
    for (size_t i = 0; i < cycChain.sortedParents.size(); ++i)
        CHECK(cycChain.sortedParents[i] < (int)i);

    // Worst-case-ordered big chain (every joint's parent comes later in the
    // array): the joint count is file-supplied, so the sort must stay
    // near-linear — the old rescan-per-emission O(n²) froze on files like this.
    const int big = 50000;
    std::vector<int> reversed(big);
    for (int i = 0; i < big; ++i)
        reversed[i] = i + 1 < big ? i + 1 : -1;
    JointOrder rev = TopoSortJoints(reversed);
    CHECK(rev.order.size() == (size_t)big);
    for (size_t i = 0; i < rev.sortedParents.size(); ++i)
        CHECK(rev.sortedParents[i] < (int)i);
    CHECK(rev.order.front() == big - 1 && rev.order.back() == 0);
}

// --- 7. weight decode + renormalize -----------------------------------------
void TestWeightDecode()
{
    // Normalized u8: 255 -> exactly 1.0 per spec.
    const uint8_t w8[4] = {255, 0, 51, 102};
    vec4 w = DecodeWeights(w8, kGltfUnsignedByte);
    CHECK(ApproxEq(w.x, 1.0f) && ApproxEq(w.y, 0.0f));
    CHECK(ApproxEq(w.z, 51.0f / 255.0f) && ApproxEq(w.w, 102.0f / 255.0f));

    const uint16_t w16[4] = {65535, 0, 13107, 26214};
    w = DecodeWeights((const uint8_t*)w16, kGltfUnsignedShort);
    CHECK(ApproxEq(w.x, 1.0f) && ApproxEq(w.z, 13107.0f / 65535.0f));

    const float wf[4] = {0.25f, 0.75f, 0.0f, 0.0f};
    w = DecodeWeights((const uint8_t*)wf, kGltfFloat);
    CHECK(ApproxEq(w.x, 0.25f) && ApproxEq(w.y, 0.75f));

    // Joint index decode across all three index widths.
    const uint8_t j8[4] = {1, 2, 3, 4};
    CHECK(DecodeJointIndices(j8, kGltfUnsignedByte) == glm::uvec4(1, 2, 3, 4));
    const uint16_t j16[4] = {300, 2, 3, 4};
    CHECK(DecodeJointIndices((const uint8_t*)j16, kGltfUnsignedShort) == glm::uvec4(300, 2, 3, 4));
    const uint32_t j32[4] = {70000, 2, 3, 4};
    CHECK(DecodeJointIndices((const uint8_t*)j32, kGltfUnsignedInt) == glm::uvec4(70000, 2, 3, 4));

    // sum != 1 renormalizes; a near-zero sum is left alone (kernel passthrough).
    vec4 renorm = RenormalizeWeights(vec4(0.5f, 0.25f, 0, 0));
    CHECK(ApproxEq(renorm.x, 2.0f / 3.0f) && ApproxEq(renorm.y, 1.0f / 3.0f));
    vec4 zero = RenormalizeWeights(vec4(1e-8f, 0, 0, 0));
    CHECK(ApproxEq(zero.x, 1e-8f, 1e-10f) && ApproxEq(zero.y, 0.0f));

    // NaN/Inf/negative components are zeroed BEFORE normalizing — a poisoned
    // component that survived would scale skinned positions by the partial
    // weight sum (spike toward the origin at bind).
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    vec4 poisoned = RenormalizeWeights(vec4(0.5f, nan, -0.3f, 0.5f));
    CHECK(ApproxEq(poisoned.x, 0.5f) && ApproxEq(poisoned.y, 0.0f));
    CHECK(ApproxEq(poisoned.z, 0.0f) && ApproxEq(poisoned.w, 0.5f));
    vec4 infinite = RenormalizeWeights(vec4(inf, 1.0f, 0, 0));
    CHECK(ApproxEq(infinite.x, 0.0f) && ApproxEq(infinite.y, 1.0f));
    vec4 allBad = RenormalizeWeights(vec4(nan, -1.0f, -inf, 0));
    CHECK(ApproxEq(allBad.x, 0.0f) && ApproxEq(allBad.y, 0.0f));
    CHECK(ApproxEq(allBad.z, 0.0f) && ApproxEq(allBad.w, 0.0f));
}

// --- 8. out-of-range joint index --------------------------------------------
void TestOutOfRangeJointIndex()
{
    // palette has 1 joint; index 5 is garbage. The bad influence is dropped
    // (never read); the valid one still contributes per the plain LBS sum —
    // it is NOT redistributed (import-time validation renormalizes; the kernel
    // only refuses to read out of bounds).
    std::vector<mat4> palette = {glm::translate(mat4(1.0f), vec3(2, 0, 0))};
    std::vector<Vertex> bind = {MakeVert({0, 0, 0}, {0, 1, 0})};
    std::vector<VertexSkin> skin = {MakeSkin({0, 5, 0, 0}, {0.5f, 0.5f, 0, 0})};
    std::vector<Vertex> out;
    SkinVertices(bind, skin, palette, out);
    CHECK(ApproxVec3(out[0].position, vec3(1, 0, 0))); // 0.5·(p + (2,0,0))

    // Every influence out of range == effective weight sum 0: bind passthrough.
    skin[0] = MakeSkin({7, 8, 9, 10}, {0.25f, 0.25f, 0.25f, 0.25f});
    SkinVertices(bind, skin, palette, out);
    CHECK(ApproxVec3(out[0].position, bind[0].position));

    // Skin table size mismatch: warned passthrough copy, still sized to bind.
    std::vector<VertexSkin> shortSkin;
    SkinVertices(bind, shortSkin, palette, out);
    CHECK(out.size() == 1);
    CHECK(ApproxVec3(out[0].position, bind[0].position));
}

} // namespace

void RunSkeletonTests()
{
    TestPaletteChain();
    TestBindPoseIdentity();
    TestBlend();
    TestRotatedNormals();
    TestNonUniformScaleNormals();
    TestTopoSortRemap();
    TestWeightDecode();
    TestOutOfRangeJointIndex();
}

} // namespace forge::test
