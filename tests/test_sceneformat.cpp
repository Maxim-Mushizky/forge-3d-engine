#include "test_framework.h"

#include <forge/assets/SceneFormat.h>

#include <cstring>
#include <string>
#include <vector>

namespace forge::test {

namespace {

SavedScene MakeReferenceScene()
{
    SavedScene s;

    SavedMesh blobMesh; // unique geometry: a single triangle
    blobMesh.vertices = {
        {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
    };
    blobMesh.indices = {0, 1, 2};
    s.meshes.push_back(blobMesh);

    SavedMesh recipeMesh;
    recipeMesh.recipe = "cube";
    s.meshes.push_back(recipeMesh);

    SavedEntity parent;
    parent.id = 42;
    parent.name = "Group";
    parent.translation = {1.0f, 2.0f, 3.0f};
    s.entities.push_back(parent);

    SavedEntity child;
    child.id = 7;
    child.parent = 42;
    child.name = "Tri";
    child.meshIndex = 0;
    child.rotation = {0.1f, 0.2f, 0.3f};
    child.scale = {2.0f, 2.0f, 2.0f};
    child.albedo = {0.25f, 0.5f, 0.75f};
    child.metallic = 0.9f;
    child.roughness = 0.1f;
    child.emissive = {1.0f, 0.5f, 0.0f};
    child.emissiveStrength = 3.5f;
    s.entities.push_back(child);

    SavedEntity lamp;
    lamp.id = 9;
    lamp.name = "Lamp";
    lamp.meshIndex = 1; // recipe mesh
    lamp.lightEnabled = true;
    lamp.lightColor = {1.0f, 0.9f, 0.8f};
    lamp.lightIntensity = 25.0f;
    lamp.lightRange = 12.0f;
    s.entities.push_back(lamp);

    s.extrasJson = R"({"sun":{"azimuth":40.0},"rt":{"bounces":4}})";
    return s;
}

bool SameVec3(const vec3& a, const vec3& b)
{
    return ApproxEq(a.x, b.x) && ApproxEq(a.y, b.y) && ApproxEq(a.z, b.z);
}

} // namespace

void RunSceneFormatTests()
{
    // --- empty scene round trip ---------------------------------------------
    {
        std::vector<uint8_t> bytes = EncodeScene(SavedScene{});
        auto back = DecodeScene(bytes.data(), bytes.size());
        CHECK(back.has_value());
        CHECK(back->entities.empty());
        CHECK(back->meshes.empty());
        CHECK(back->extrasJson.empty());
    }

    // --- full round trip: entities, hierarchy, materials, lights, meshes ------
    {
        SavedScene ref = MakeReferenceScene();
        std::vector<uint8_t> bytes = EncodeScene(ref);
        auto back = DecodeScene(bytes.data(), bytes.size());
        CHECK(back.has_value());
        if (back) {
            CHECK(back->entities.size() == 3);
            CHECK(back->meshes.size() == 2);

            const SavedEntity& parent = back->entities[0];
            CHECK(parent.id == 42);
            CHECK(parent.parent == 0);
            CHECK(parent.name == "Group");
            CHECK(parent.meshIndex == -1);
            CHECK(SameVec3(parent.translation, {1.0f, 2.0f, 3.0f}));

            const SavedEntity& child = back->entities[1];
            CHECK(child.parent == 42);
            CHECK(child.meshIndex == 0);
            CHECK(SameVec3(child.rotation, {0.1f, 0.2f, 0.3f}));
            CHECK(SameVec3(child.albedo, {0.25f, 0.5f, 0.75f}));
            CHECK(ApproxEq(child.metallic, 0.9f));
            CHECK(ApproxEq(child.roughness, 0.1f));
            CHECK(ApproxEq(child.emissiveStrength, 3.5f));
            CHECK(!child.lightEnabled);

            const SavedEntity& lamp = back->entities[2];
            CHECK(lamp.lightEnabled);
            CHECK(ApproxEq(lamp.lightIntensity, 25.0f));
            CHECK(ApproxEq(lamp.lightRange, 12.0f));
            CHECK(lamp.meshIndex == 1);

            // blob mesh: exact byte-level vertex round trip
            const SavedMesh& tri = back->meshes[0];
            CHECK(tri.recipe.empty());
            CHECK(tri.vertices.size() == 3);
            CHECK(tri.indices.size() == 3);
            CHECK(std::memcmp(tri.vertices.data(), ref.meshes[0].vertices.data(),
                              3 * sizeof(Vertex)) == 0);
            CHECK(tri.indices == ref.meshes[0].indices);

            // recipe mesh: id preserved, no blob
            CHECK(back->meshes[1].recipe == "cube");
            CHECK(back->meshes[1].vertices.empty());

            // extras survive (round-tripped through parsed json, so compare keys)
            CHECK(back->extrasJson.find("azimuth") != std::string::npos);
            CHECK(back->extrasJson.find("bounces") != std::string::npos);
        }
    }

    // --- corrupt input: must return nullopt, never crash ----------------------
    {
        SavedScene ref = MakeReferenceScene();
        std::vector<uint8_t> bytes = EncodeScene(ref);

        CHECK(!DecodeScene(nullptr, 0).has_value());
        CHECK(!DecodeScene(bytes.data(), 4).has_value()); // shorter than the header

        std::vector<uint8_t> badMagic = bytes;
        badMagic[0] = 'X';
        CHECK(!DecodeScene(badMagic.data(), badMagic.size()).has_value());

        std::vector<uint8_t> newerVersion = bytes;
        newerVersion[8] = (uint8_t)(kSceneFormatVersion + 1); // refuse future formats
        CHECK(!DecodeScene(newerVersion.data(), newerVersion.size()).has_value());

        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
        CHECK(!DecodeScene(truncated.data(), truncated.size()).has_value());

        std::vector<uint8_t> junkJson = bytes;
        for (size_t i = 16; i < 24 && i < junkJson.size(); ++i)
            junkJson[i] = '#';
        CHECK(!DecodeScene(junkJson.data(), junkJson.size()).has_value());
    }

    // --- hostile blob reference: offset/count past the blob section -----------
    {
        // Hand-build a header whose mesh blob points beyond the file.
        std::string json = R"({"version":1,"entities":[],"meshes":[)"
                           R"({"vertexCount":1000,"indexCount":3,"offset":0}]})";
        std::vector<uint8_t> bytes;
        const char magic[8] = {'F', 'O', 'R', 'G', 'E', 'S', 'C', 'N'};
        bytes.insert(bytes.end(), magic, magic + 8);
        uint32_t version = 1, len = (uint32_t)json.size();
        bytes.insert(bytes.end(), (uint8_t*)&version, (uint8_t*)&version + 4);
        bytes.insert(bytes.end(), (uint8_t*)&len, (uint8_t*)&len + 4);
        bytes.insert(bytes.end(), json.begin(), json.end());
        bytes.resize(bytes.size() + 16, 0); // 16-byte blob, far less than promised
        CHECK(!DecodeScene(bytes.data(), bytes.size()).has_value());
    }

    // --- overflow attack: vertexCount near 2^64 must not wrap the byte sum ----
    {
        std::string json = R"({"version":1,"entities":[],"meshes":[)"
                           R"({"vertexCount":576460752303423488,"indexCount":4,"offset":0}]})";
        std::vector<uint8_t> bytes;
        const char magic[8] = {'F', 'O', 'R', 'G', 'E', 'S', 'C', 'N'};
        bytes.insert(bytes.end(), magic, magic + 8);
        uint32_t version = 1, len = (uint32_t)json.size();
        bytes.insert(bytes.end(), (uint8_t*)&version, (uint8_t*)&version + 4);
        bytes.insert(bytes.end(), (uint8_t*)&len, (uint8_t*)&len + 4);
        bytes.insert(bytes.end(), json.begin(), json.end());
        bytes.resize(bytes.size() + 64, 0);
        CHECK(!DecodeScene(bytes.data(), bytes.size()).has_value());
    }

    // --- forward compatibility: unknown keys ignored, defaults applied --------
    {
        std::string json = R"({"version":1,"futureFeature":{"x":1},)"
                           R"("entities":[{"id":5,"name":"N","newKey":true}],"meshes":[]})";
        std::vector<uint8_t> bytes;
        const char magic[8] = {'F', 'O', 'R', 'G', 'E', 'S', 'C', 'N'};
        bytes.insert(bytes.end(), magic, magic + 8);
        uint32_t version = 1, len = (uint32_t)json.size();
        bytes.insert(bytes.end(), (uint8_t*)&version, (uint8_t*)&version + 4);
        bytes.insert(bytes.end(), (uint8_t*)&len, (uint8_t*)&len + 4);
        bytes.insert(bytes.end(), json.begin(), json.end());

        auto back = DecodeScene(bytes.data(), bytes.size());
        CHECK(back.has_value());
        if (back) {
            CHECK(back->entities.size() == 1);
            CHECK(back->entities[0].id == 5);
            CHECK(back->entities[0].name == "N");
            CHECK(back->entities[0].meshIndex == -1);
            CHECK(SameVec3(back->entities[0].scale, {1.0f, 1.0f, 1.0f})); // default
        }
    }

    // --- dangling mesh index rejected ------------------------------------------
    {
        std::string json = R"({"version":1,"entities":[{"id":1,"mesh":3}],"meshes":[]})";
        std::vector<uint8_t> bytes;
        const char magic[8] = {'F', 'O', 'R', 'G', 'E', 'S', 'C', 'N'};
        bytes.insert(bytes.end(), magic, magic + 8);
        uint32_t version = 1, len = (uint32_t)json.size();
        bytes.insert(bytes.end(), (uint8_t*)&version, (uint8_t*)&version + 4);
        bytes.insert(bytes.end(), (uint8_t*)&len, (uint8_t*)&len + 4);
        bytes.insert(bytes.end(), json.begin(), json.end());
        CHECK(!DecodeScene(bytes.data(), bytes.size()).has_value());
    }
}

} // namespace forge::test
