#include "test_framework.h"

#include <forge/anim/Pose.h>
#include <forge/anim/PosePresets.h>
#include <forge/anim/Skeleton.h>
#include <forge/anim/Skinning.h>
#include <forge/assets/SceneFormat.h>

#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace forge::test {

namespace {

constexpr float kHalfPi = 1.57079632679f;

bool ApproxVec3(const vec3& a, const vec3& b, float eps = 1e-4f)
{
    return ApproxEq(a.x, b.x, eps) && ApproxEq(a.y, b.y, eps) && ApproxEq(a.z, b.z, eps);
}

bool ApproxQuat(const quat& a, const quat& b, float eps = 1e-5f)
{
    return ApproxEq(a.x, b.x, eps) && ApproxEq(a.y, b.y, eps) && ApproxEq(a.z, b.z, eps) &&
           ApproxEq(a.w, b.w, eps);
}

bool ApproxMat4(const mat4& a, const mat4& b, float eps = 1e-6f)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!ApproxEq(a[c][r], b[c][r], eps))
                return false;
    return true;
}

bool IsIdentity(const quat& q, float eps = 1e-5f)
{
    return ApproxQuat(q, quat(1.0f, 0.0f, 0.0f, 0.0f), eps);
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

// Two joints: root at origin, child at +Y*1, identity rotations, unit scale,
// exact inverse-of-bind-global IBMs.
Skeleton MakeTwoJointChain()
{
    Skeleton sk;
    sk.parents = {-1, 0};
    sk.names = {"root", "child"};
    sk.bindT = {vec3(0.0f), vec3(0, 1, 0)};
    sk.bindR.assign(2, quat(1.0f, 0.0f, 0.0f, 0.0f));
    sk.bindS.assign(2, vec3(1.0f));
    for (const mat4& g : ComputeBindGlobals(sk))
        sk.inverseBind.push_back(glm::inverse(g));
    return sk;
}

// Hand-build a .forge byte stream around a raw json header (hostile-input rig).
std::vector<uint8_t> BuildFile(const std::string& header, size_t blobBytes)
{
    std::vector<uint8_t> bytes;
    const char magic[8] = {'F', 'O', 'R', 'G', 'E', 'S', 'C', 'N'};
    bytes.insert(bytes.end(), magic, magic + 8);
    uint32_t version = kSceneFormatVersion, len = (uint32_t)header.size();
    bytes.insert(bytes.end(), (uint8_t*)&version, (uint8_t*)&version + 4);
    bytes.insert(bytes.end(), (uint8_t*)&len, (uint8_t*)&len + 4);
    bytes.insert(bytes.end(), header.begin(), header.end());
    bytes.resize(bytes.size() + blobBytes, 0);
    return bytes;
}

// --- 1. JointIndex ------------------------------------------------------------
void TestJointIndex()
{
    Skeleton sk;
    sk.parents = {-1, 0, 1};
    sk.names = {"root", "spine", "arm"};
    CHECK(JointIndex(sk, "root") == 0);
    CHECK(JointIndex(sk, "spine") == 1);
    CHECK(JointIndex(sk, "arm") == 2);
    CHECK(JointIndex(sk, "missing") == -1);
    CHECK(JointIndex(sk, "") == -1);

    Skeleton empty;
    CHECK(JointIndex(empty, "root") == -1);
}

// --- 2. PoseLocalRotations -----------------------------------------------------
void TestPoseLocalRotations()
{
    Skeleton sk;
    sk.parents = {-1, 0, 1};
    sk.names = {"a", "b", "c"};
    sk.bindT.assign(3, vec3(0.0f));
    sk.bindR = {glm::angleAxis(0.3f, vec3(0, 0, 1)),
                glm::angleAxis(0.7f, glm::normalize(vec3(1, 2, 0))),
                quat(1.0f, 0.0f, 0.0f, 0.0f)};
    sk.bindS.assign(3, vec3(1.0f));

    // Empty pose: bindR verbatim (the copy path, exact).
    std::vector<quat> r = PoseLocalRotations(sk, Pose{});
    CHECK(r.size() == 3);
    for (size_t i = 0; i < 3; ++i)
        CHECK(ApproxQuat(r[i], sk.bindR[i], 0.0f));

    // Mis-sized deltas (1 for a 3-joint rig): degrade to bind, no crash.
    Pose misSized;
    misSized.deltas = {glm::angleAxis(kHalfPi, vec3(1, 0, 0))};
    r = PoseLocalRotations(sk, misSized);
    for (size_t i = 0; i < 3; ++i)
        CHECK(ApproxQuat(r[i], sk.bindR[i], 0.0f));

    // All-identity deltas: bindR within 1e-5 (normalize may nudge components).
    Pose identity;
    identity.deltas.assign(3, quat(1.0f, 0.0f, 0.0f, 0.0f));
    r = PoseLocalRotations(sk, identity);
    for (size_t i = 0; i < 3; ++i)
        CHECK(ApproxQuat(r[i], sk.bindR[i]));

    // One non-identity delta on joint 1: result[1] == normalize(bindR[1]*delta),
    // the others stay at bind.
    Pose bent = identity;
    const quat delta = glm::angleAxis(kHalfPi, vec3(0, 0, 1));
    bent.deltas[1] = delta;
    r = PoseLocalRotations(sk, bent);
    CHECK(ApproxQuat(r[0], sk.bindR[0]));
    CHECK(ApproxQuat(r[1], glm::normalize(sk.bindR[1] * delta)));
    CHECK(ApproxQuat(r[2], sk.bindR[2]));
}

// --- 3. end-to-end pose skinning (pure) ----------------------------------------
void TestPoseSkinningEndToEnd()
{
    Skeleton sk = MakeTwoJointChain();
    std::vector<Vertex> bind = {
        MakeVert({0.5f, 0.2f, 0.0f}, {0, 0, 1}), // 100% root
        MakeVert({0.3f, 1.5f, 0.0f}, {0, 0, 1}), // 100% child
    };
    std::vector<VertexSkin> skin = {
        MakeSkin({0, 0, 0, 0}, {1, 0, 0, 0}),
        MakeSkin({1, 0, 0, 0}, {1, 0, 0, 0}),
    };

    // Empty pose == bind: the deform is an exact identity.
    auto deform = [&](const Pose& pose) {
        std::vector<quat> r = PoseLocalRotations(sk, pose);
        std::vector<mat4> globals = ComputeGlobalTransforms(sk, sk.bindT, r, sk.bindS);
        std::vector<mat4> palette = ComputePalette(globals, sk.inverseBind);
        std::vector<Vertex> out;
        SkinVertices(bind, skin, palette, out);
        return out;
    };
    std::vector<Vertex> out = deform(Pose{});
    CHECK(out.size() == 2);
    CHECK(ApproxVec3(out[0].position, bind[0].position));
    CHECK(ApproxVec3(out[1].position, bind[1].position));

    // +90° about Z on the child joint. Hand-computed expectation:
    //   childGlobal = T(0,1,0) * Rz(90); palette = childGlobal * inverseBind[1];
    //   expected    = palette * bindPos  ->  (-0.5, 1.3, 0) for (0.3, 1.5, 0).
    Pose bent;
    bent.deltas = {quat(1.0f, 0.0f, 0.0f, 0.0f), glm::angleAxis(kHalfPi, vec3(0, 0, 1))};
    out = deform(bent);
    const mat4 childGlobal = glm::translate(mat4(1.0f), vec3(0, 1, 0)) *
                             glm::mat4_cast(glm::angleAxis(kHalfPi, vec3(0, 0, 1)));
    const vec3 expected = vec3(childGlobal * sk.inverseBind[1] * vec4(bind[1].position, 1.0f));
    CHECK(ApproxVec3(out[1].position, expected));
    CHECK(ApproxVec3(out[1].position, vec3(-0.5f, 1.3f, 0.0f)));
    // The root-weighted vertex must not move.
    CHECK(ApproxVec3(out[0].position, bind[0].position));
}

// --- 4. serialization round-trip -----------------------------------------------
void TestPoseSerializationRoundTrip()
{
    SavedScene ref;

    SavedMesh mesh;
    mesh.vertices = {
        MakeVert({0, 0, 0}, {0, 1, 0}, {0.0f, 0.0f}),
        MakeVert({0, 1, 0}, {0, 1, 0}, {0.5f, 0.5f}),
        MakeVert({0, 2, 0}, {0, 1, 0}, {1.0f, 1.0f}),
    };
    mesh.indices = {0, 1, 2};
    mesh.skin = {
        MakeSkin({0, 1, 0, 0}, {0.7f, 0.3f, 0.0f, 0.0f}),
        MakeSkin({1, 0, 0, 0}, {1.0f, 0.0f, 0.0f, 0.0f}),
        MakeSkin({0, 1, 2, 3}, {0.25f, 0.25f, 0.25f, 0.25f}),
    };
    ref.meshes.push_back(mesh);

    SavedEntity e;
    e.id = 11;
    e.name = "Rigged";
    e.meshIndex = 0;
    e.skeleton.parents = {-1, 0};
    e.skeleton.names = {"root", "child"};
    e.skeleton.bindT = {vec3(0.0f), vec3(0, 1, 0)};
    e.skeleton.bindR = {quat(1.0f, 0.0f, 0.0f, 0.0f),
                        glm::angleAxis(0.5f, glm::normalize(vec3(0, 0, 1)))};
    e.skeleton.bindS = {vec3(1.0f), vec3(2, 1, 1)};
    e.skeleton.inverseBind = {mat4(1.0f), glm::translate(mat4(1.0f), vec3(0, -1, 0))};
    e.pose = {glm::angleAxis(0.25f, vec3(1, 0, 0)), quat(1.0f, 0.0f, 0.0f, 0.0f)};
    ref.entities.push_back(e);

    std::vector<uint8_t> bytes = EncodeScene(ref);
    auto back = DecodeScene(bytes.data(), bytes.size());
    CHECK(back.has_value());
    if (back) {
        CHECK(back->meshes.size() == 1);
        const SavedMesh& m = back->meshes[0];
        CHECK(m.vertices.size() == 3 && m.indices.size() == 3);
        // Skin survives byte-exact: raw blob copy, same as vertices.
        CHECK(m.skin.size() == 3);
        if (m.skin.size() == 3)
            CHECK(std::memcmp(m.skin.data(), ref.meshes[0].skin.data(),
                              3 * sizeof(VertexSkin)) == 0);

        CHECK(back->entities.size() == 1);
        const SavedEntity& be = back->entities[0];
        CHECK(!be.skeleton.Empty());
        CHECK(be.skeleton.parents == e.skeleton.parents);
        CHECK(be.skeleton.names == e.skeleton.names);
        CHECK(be.skeleton.bindT.size() == 2 && be.skeleton.bindR.size() == 2 &&
              be.skeleton.bindS.size() == 2 && be.skeleton.inverseBind.size() == 2);
        for (size_t i = 0; i < 2; ++i) {
            CHECK(ApproxVec3(be.skeleton.bindT[i], e.skeleton.bindT[i], 1e-6f));
            CHECK(ApproxQuat(be.skeleton.bindR[i], e.skeleton.bindR[i], 1e-6f));
            CHECK(ApproxVec3(be.skeleton.bindS[i], e.skeleton.bindS[i], 1e-6f));
            CHECK(ApproxMat4(be.skeleton.inverseBind[i], e.skeleton.inverseBind[i]));
        }
        CHECK(be.pose.size() == 2);
        for (size_t i = 0; i < be.pose.size() && i < e.pose.size(); ++i)
            CHECK(ApproxQuat(be.pose[i], e.pose[i], 1e-6f));
    }

    // Back-compat: a scene with NO skin/skeleton/pose round-trips unchanged and
    // decodes with those fields empty (pre-v3 files behave the same way).
    SavedScene plain;
    SavedMesh pm;
    pm.vertices = {MakeVert({0, 0, 0}, {0, 1, 0})};
    pm.indices = {0, 0, 0};
    plain.meshes.push_back(pm);
    SavedEntity pe;
    pe.id = 4;
    pe.name = "Plain";
    pe.meshIndex = 0;
    plain.entities.push_back(pe);
    std::vector<uint8_t> plainBytes = EncodeScene(plain);
    auto plainBack = DecodeScene(plainBytes.data(), plainBytes.size());
    CHECK(plainBack.has_value());
    if (plainBack) {
        CHECK(plainBack->meshes[0].skin.empty());
        CHECK(plainBack->entities[0].skeleton.Empty());
        CHECK(plainBack->entities[0].pose.empty());
        CHECK(plainBack->meshes[0].vertices.size() == 1);
    }
}

// --- 5. hostile skin blob ------------------------------------------------------
void TestSkinBlobGuards()
{
    // 3 verts (96 B) + 3 indices (12 B) = 108 B of legitimate blob. A skin that
    // promises 3 entries (96 B) at offset 108 needs 204 B — reject, never read.
    {
        std::string header =
            R"({"version":3,"entities":[],"meshes":[{"vertexCount":3,"indexCount":3,)"
            R"("offset":0,"skinCount":3,"skinOffset":108}]})";
        std::vector<uint8_t> bytes = BuildFile(header, 108);
        CHECK(!DecodeScene(bytes.data(), bytes.size()).has_value());
    }
    // Overflow attack on skinCount: a count near 2^64 must not wrap the byte sum.
    {
        std::string header =
            R"({"version":3,"entities":[],"meshes":[{"vertexCount":3,"indexCount":3,)"
            R"("offset":0,"skinCount":576460752303423488,"skinOffset":0}]})";
        std::vector<uint8_t> bytes = BuildFile(header, 256);
        CHECK(!DecodeScene(bytes.data(), bytes.size()).has_value());
    }
    // skinCount != vertexCount (in-range but corrupt): drop the skin, keep the
    // mesh — renders unskinned instead of risking OOB joint reads later.
    {
        std::string header =
            R"({"version":3,"entities":[],"meshes":[{"vertexCount":3,"indexCount":3,)"
            R"("offset":0,"skinCount":2,"skinOffset":108}]})";
        std::vector<uint8_t> bytes = BuildFile(header, 108 + 2 * sizeof(VertexSkin));
        auto back = DecodeScene(bytes.data(), bytes.size());
        CHECK(back.has_value());
        if (back) {
            CHECK(back->meshes.size() == 1);
            CHECK(back->meshes[0].vertices.size() == 3);
            CHECK(back->meshes[0].skin.empty());
        }
    }
}

// --- 6. canned presets ----------------------------------------------------------
void TestPresetPoses()
{
    Skeleton sk;
    sk.parents = {-1, 0, 0, 0};
    sk.names = {"Hips", "LeftArm", "RightArm", "LeftUpLeg"};
    sk.bindT.assign(4, vec3(0.0f));
    sk.bindR.assign(4, quat(1.0f, 0.0f, 0.0f, 0.0f));
    sk.bindS.assign(4, vec3(1.0f));

    // "rest": full-size all-identity deltas — a real pose that clears overrides.
    std::optional<Pose> rest = MakePresetPose(sk, "rest");
    CHECK(rest.has_value());
    if (rest) {
        CHECK(rest->deltas.size() == sk.JointCount());
        for (const quat& q : rest->deltas)
            CHECK(IsIdentity(q));
    }

    // Unknown preset name: nullopt, not a silent no-op.
    CHECK(!MakePresetPose(sk, "flying").has_value());
    CHECK(!MakePresetPose(sk, "").has_value());

    // "a-pose": non-identity deltas on the matching arm joints ONLY.
    std::optional<Pose> aPose = MakePresetPose(sk, "a-pose");
    CHECK(aPose.has_value());
    if (aPose) {
        CHECK(aPose->deltas.size() == 4);
        CHECK(IsIdentity(aPose->deltas[0]));  // Hips untouched
        CHECK(!IsIdentity(aPose->deltas[1])); // LeftArm bent
        CHECK(!IsIdentity(aPose->deltas[2])); // RightArm bent
        CHECK(IsIdentity(aPose->deltas[3]));  // LeftUpLeg untouched
    }

    // "sit" on this rig only matches LeftUpLeg — the missing leg joints skip.
    std::optional<Pose> sit = MakePresetPose(sk, "sit");
    CHECK(sit.has_value());
    if (sit) {
        CHECK(IsIdentity(sit->deltas[0]) && IsIdentity(sit->deltas[1]) &&
              IsIdentity(sit->deltas[2]));
        CHECK(!IsIdentity(sit->deltas[3]));
    }

    // A rig with none of the standard names: every preset is a harmless no-op
    // (all-identity), never an error.
    Skeleton tube;
    tube.parents = {-1, 0};
    tube.names = {"spine_0", "spine_1"};
    tube.bindT.assign(2, vec3(0.0f));
    tube.bindR.assign(2, quat(1.0f, 0.0f, 0.0f, 0.0f));
    tube.bindS.assign(2, vec3(1.0f));
    std::optional<Pose> noop = MakePresetPose(tube, "sit");
    CHECK(noop.has_value());
    if (noop) {
        CHECK(noop->deltas.size() == 2);
        for (const quat& q : noop->deltas)
            CHECK(IsIdentity(q));
    }
}

// --- 7. hostile non-finite skeleton/pose values -------------------------------
// A finite-but-huge file number (1e40) narrows to +Inf on the float cast, and
// glm::normalize does not guard Inf/NaN — it would cascade through the joint
// palette and NaN the whole skinned mesh. The loader must clamp such values to a
// benign default (JsonToQuat / JsonToMat4), matching the ragged-skeleton / skin-
// blob hostile-file posture.
void TestPoseHostileValues()
{
    // A pose delta with an out-of-float-range component decodes to identity and
    // stays finite (would otherwise Inf -> NaN the palette on load).
    {
        std::string header =
            R"({"version":3,"entities":[{"id":1,"name":"h","pose":[[1e40,0,0,1]]}],"meshes":[]})";
        std::vector<uint8_t> bytes = BuildFile(header, 0);
        auto back = DecodeScene(bytes.data(), bytes.size());
        CHECK(back.has_value());
        if (back && back->entities.size() == 1 && back->entities[0].pose.size() == 1) {
            const quat q = back->entities[0].pose[0];
            CHECK(std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) &&
                  std::isfinite(q.w));
            CHECK(IsIdentity(q));
        }
    }
    // A skeleton bindR with a huge component -> identity quat; a huge inverseBind
    // element -> identity matrix (both feed the palette directly).
    {
        std::string header =
            R"({"version":3,"entities":[{"id":1,"name":"h","skeleton":{)"
            R"("parents":[-1],"names":["r"],"bindT":[[0,0,0]],"bindR":[[1e40,0,0,1]],)"
            R"("bindS":[[1,1,1]],"inverseBind":[[9e40,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]]}}],"meshes":[]})";
        std::vector<uint8_t> bytes = BuildFile(header, 0);
        auto back = DecodeScene(bytes.data(), bytes.size());
        CHECK(back.has_value());
        if (back && back->entities.size() == 1) {
            const SavedSkeleton& s = back->entities[0].skeleton;
            CHECK(!s.Empty());
            if (s.bindR.size() == 1)
                CHECK(IsIdentity(s.bindR[0]));
            if (s.inverseBind.size() == 1)
                CHECK(ApproxMat4(s.inverseBind[0], mat4(1.0f)));
        }
    }
}

} // namespace

void RunPoseTests()
{
    TestJointIndex();
    TestPoseLocalRotations();
    TestPoseSkinningEndToEnd();
    TestPoseSerializationRoundTrip();
    TestSkinBlobGuards();
    TestPresetPoses();
    TestPoseHostileValues();
}

} // namespace forge::test
