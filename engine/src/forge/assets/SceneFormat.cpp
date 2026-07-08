#include "SceneFormat.h"

#include <json.hpp> // nlohmann, bundled with tinygltf

#include <cstring>

namespace forge {

using nlohmann::json;

static_assert(sizeof(Vertex) == 8 * sizeof(float), "Vertex layout changed - bump kSceneFormatVersion");
static_assert(sizeof(VertexSkin) == 8 * sizeof(float), "VertexSkin layout changed - bump kSceneFormatVersion");

namespace {

constexpr char kMagic[8] = {'F', 'O', 'R', 'G', 'E', 'S', 'C', 'N'};

json Vec3ToJson(const vec3& v) { return json::array({v.x, v.y, v.z}); }

vec3 JsonToVec3(const json& j, const vec3& fallback)
{
    if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number())
        return fallback;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

json QuatToJson(const quat& q) { return json::array({q.x, q.y, q.z, q.w}); } // xyzw

quat JsonToQuat(const json& j)
{
    if (!j.is_array() || j.size() != 4 || !j[0].is_number() || !j[1].is_number() ||
        !j[2].is_number() || !j[3].is_number())
        return quat(1, 0, 0, 0);
    return quat(j[3].get<float>(), j[0].get<float>(), j[1].get<float>(), j[2].get<float>()); // (w,x,y,z)
}

json Mat4ToJson(const mat4& m)
{
    json a = json::array();
    const float* p = &m[0][0];
    for (int i = 0; i < 16; ++i)
        a.push_back(p[i]); // column-major, glm's native order
    return a;
}

mat4 JsonToMat4(const json& j)
{
    mat4 m(1.0f);
    if (!j.is_array() || j.size() != 16)
        return m;
    float* p = &m[0][0];
    for (int i = 0; i < 16; ++i)
        p[i] = j[i].is_number() ? j[i].get<float>() : p[i];
    return m;
}

template <typename T>
T GetOr(const json& j, const char* key, T fallback)
{
    auto it = j.find(key);
    if (it == j.end())
        return fallback;
    if constexpr (std::is_same_v<T, std::string>) {
        return it->is_string() ? it->get<std::string>() : fallback;
    } else if constexpr (std::is_same_v<T, bool>) {
        return it->is_boolean() ? it->get<bool>() : fallback;
    } else {
        return it->is_number() ? it->get<T>() : fallback;
    }
}

void Append(std::vector<uint8_t>& out, const void* data, size_t bytes)
{
    if (bytes == 0)
        return; // empty vectors may hand out a null data() — don't form pointers from it
    const uint8_t* p = (const uint8_t*)data;
    out.insert(out.end(), p, p + bytes);
}

} // namespace

std::vector<uint8_t> EncodeScene(const SavedScene& scene)
{
    // Blob first so the json can reference offsets.
    std::vector<uint8_t> blob;
    json meshes = json::array();
    for (const SavedMesh& m : scene.meshes) {
        json jm;
        if (!m.recipe.empty()) {
            jm["recipe"] = m.recipe;
        } else {
            jm["vertexCount"] = (uint64_t)m.vertices.size();
            jm["indexCount"] = (uint64_t)m.indices.size();
            jm["offset"] = (uint64_t)blob.size();
            Append(blob, m.vertices.data(), m.vertices.size() * sizeof(Vertex));
            Append(blob, m.indices.data(), m.indices.size() * sizeof(uint32_t));
            if (!m.skin.empty()) {
                jm["skinCount"] = (uint64_t)m.skin.size();
                jm["skinOffset"] = (uint64_t)blob.size();
                Append(blob, m.skin.data(), m.skin.size() * sizeof(VertexSkin));
            }
            if (!m.submeshes.empty()) {
                json subs = json::array();
                for (const Submesh& s : m.submeshes)
                    subs.push_back(json::array({s.firstIndex, s.indexCount, s.materialSlot}));
                jm["submeshes"] = std::move(subs); // [first, count, slot] triples (v2, #80)
            }
        }
        meshes.push_back(std::move(jm));
    }

    json entities = json::array();
    for (const SavedEntity& e : scene.entities) {
        json je;
        je["id"] = e.id;
        if (e.parent)
            je["parent"] = e.parent;
        je["name"] = e.name;
        je["translation"] = Vec3ToJson(e.translation);
        je["rotation"] = Vec3ToJson(e.rotation);
        je["scale"] = Vec3ToJson(e.scale);
        if (e.meshIndex >= 0)
            je["mesh"] = e.meshIndex;
        je["albedo"] = Vec3ToJson(e.albedo);
        je["metallic"] = e.metallic;
        je["roughness"] = e.roughness;
        je["emissive"] = Vec3ToJson(e.emissive);
        je["emissiveStrength"] = e.emissiveStrength;
        je["transmission"] = e.transmission;
        je["ior"] = e.ior;
        je["subsurface"] = e.subsurface; // SSS (#112, optional keys)
        je["subsurfaceColor"] = Vec3ToJson(e.subsurfaceColor);
        je["subsurfaceRadius"] = Vec3ToJson(e.subsurfaceRadius);
        if (!e.albedoSource.empty())
            je["albedoSource"] = e.albedoSource; // texture sources (#113, optional keys)
        if (!e.mrSource.empty())
            je["mrSource"] = e.mrSource;
        if (!e.extraMaterials.empty()) {
            json mats = json::array();
            for (const SavedMaterial& m : e.extraMaterials) {
                json jm = {{"albedo", Vec3ToJson(m.albedo)},
                           {"metallic", m.metallic},
                           {"roughness", m.roughness},
                           {"emissive", Vec3ToJson(m.emissive)},
                           {"emissiveStrength", m.emissiveStrength},
                           {"transmission", m.transmission},
                           {"ior", m.ior},
                           {"subsurface", m.subsurface},
                           {"subsurfaceColor", Vec3ToJson(m.subsurfaceColor)},
                           {"subsurfaceRadius", Vec3ToJson(m.subsurfaceRadius)}};
                if (!m.albedoSource.empty())
                    jm["albedoSource"] = m.albedoSource;
                if (!m.mrSource.empty())
                    jm["mrSource"] = m.mrSource;
                mats.push_back(std::move(jm));
            }
            je["extraMaterials"] = std::move(mats); // material slots 1+ (v2, #80)
        }
        if (e.lightEnabled) {
            je["light"] = {{"color", Vec3ToJson(e.lightColor)},
                           {"intensity", e.lightIntensity},
                           {"range", e.lightRange}};
        }
        if (!e.skeleton.Empty()) { // rig persists inline (v3, #147)
            const SavedSkeleton& s = e.skeleton;
            json js;
            js["parents"] = s.parents;
            js["names"] = s.names;
            json bt = json::array(), br = json::array(), bs = json::array(), ib = json::array();
            for (const vec3& v : s.bindT)
                bt.push_back(Vec3ToJson(v));
            for (const quat& q : s.bindR)
                br.push_back(QuatToJson(q));
            for (const vec3& v : s.bindS)
                bs.push_back(Vec3ToJson(v));
            for (const mat4& m : s.inverseBind)
                ib.push_back(Mat4ToJson(m));
            js["bindT"] = std::move(bt);
            js["bindR"] = std::move(br);
            js["bindS"] = std::move(bs);
            js["inverseBind"] = std::move(ib);
            je["skeleton"] = std::move(js);
        }
        if (!e.pose.empty()) {
            json jp = json::array();
            for (const quat& q : e.pose)
                jp.push_back(QuatToJson(q));
            je["pose"] = std::move(jp); // per-joint deltas (v3, #147)
        }
        entities.push_back(std::move(je));
    }

    json root;
    root["version"] = kSceneFormatVersion;
    root["entities"] = std::move(entities);
    root["meshes"] = std::move(meshes);
    if (!scene.extrasJson.empty()) {
        json extras = json::parse(scene.extrasJson, nullptr, /*allow_exceptions=*/false);
        if (!extras.is_discarded())
            root["extras"] = std::move(extras);
    }

    std::string header = root.dump();

    std::vector<uint8_t> out;
    out.reserve(sizeof(kMagic) + 8 + header.size() + blob.size());
    Append(out, kMagic, sizeof(kMagic));
    uint32_t version = kSceneFormatVersion;
    uint32_t jsonLen = (uint32_t)header.size();
    Append(out, &version, 4);
    Append(out, &jsonLen, 4);
    Append(out, header.data(), header.size());
    Append(out, blob.data(), blob.size());
    return out;
}

std::optional<SavedScene> DecodeScene(const uint8_t* data, size_t size)
{
    if (!data || size < sizeof(kMagic) + 8)
        return std::nullopt;
    if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0)
        return std::nullopt;

    uint32_t version = 0, jsonLen = 0;
    std::memcpy(&version, data + 8, 4);
    std::memcpy(&jsonLen, data + 12, 4);
    if (version == 0 || version > kSceneFormatVersion)
        return std::nullopt; // newer files refuse to half-load
    if ((size_t)jsonLen > size - 16)
        return std::nullopt;

    json root = json::parse(data + 16, data + 16 + jsonLen, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded() || !root.is_object())
        return std::nullopt;

    const uint8_t* blob = data + 16 + jsonLen;
    const size_t blobSize = size - 16 - jsonLen;

    SavedScene scene;

    if (auto it = root.find("meshes"); it != root.end() && it->is_array()) {
        for (const json& jm : *it) {
            if (!jm.is_object())
                return std::nullopt;
            SavedMesh m;
            m.recipe = GetOr<std::string>(jm, "recipe", "");
            if (m.recipe.empty()) {
                uint64_t vCount = GetOr<uint64_t>(jm, "vertexCount", 0);
                uint64_t iCount = GetOr<uint64_t>(jm, "indexCount", 0);
                uint64_t offset = GetOr<uint64_t>(jm, "offset", 0);
                // Per-count division checks first: a hostile vCount near 2^64
                // would overflow the byte sum and sneak past a single check.
                if (vCount > blobSize / sizeof(Vertex) || iCount > blobSize / sizeof(uint32_t))
                    return std::nullopt;
                uint64_t bytes = vCount * sizeof(Vertex) + iCount * sizeof(uint32_t);
                if (offset > blobSize || bytes > blobSize - offset)
                    return std::nullopt; // truncated or hostile blob reference
                m.vertices.resize(vCount);
                m.indices.resize(iCount);
                if (vCount)
                    std::memcpy(m.vertices.data(), blob + offset, vCount * sizeof(Vertex));
                if (iCount)
                    std::memcpy(m.indices.data(), blob + offset + vCount * sizeof(Vertex),
                                iCount * sizeof(uint32_t));
                uint64_t skinCount = GetOr<uint64_t>(jm, "skinCount", 0); // v3 skin (#147)
                if (skinCount) {
                    uint64_t skinOffset = GetOr<uint64_t>(jm, "skinOffset", 0);
                    // Per-count check first (overflow-safe), then range; mirror the vertex guard.
                    if (skinCount > blobSize / sizeof(VertexSkin))
                        return std::nullopt;
                    uint64_t skinBytes = skinCount * sizeof(VertexSkin);
                    if (skinOffset > blobSize || skinBytes > blobSize - skinOffset)
                        return std::nullopt;
                    // Skin must be parallel to vertices; a mismatch is a corrupt file —
                    // drop the skin (renders unskinned) rather than risk an OOB in SkinVertices.
                    if (skinCount == vCount) {
                        m.skin.resize(skinCount);
                        std::memcpy(m.skin.data(), blob + skinOffset, skinBytes);
                    }
                }
                if (auto st = jm.find("submeshes"); st != jm.end() && st->is_array()) {
                    std::vector<Submesh> subs;
                    for (const json& js : *st) {
                        if (!js.is_array() || js.size() != 3 || !js[0].is_number_unsigned() ||
                            !js[1].is_number_unsigned() || !js[2].is_number_unsigned())
                            continue; // lenient on content: skip malformed triples
                        // Read wide first: get<uint32_t> would silently truncate a
                        // >2^32 value into a plausible-looking small range.
                        uint64_t f = js[0].get<uint64_t>(), c = js[1].get<uint64_t>(), s = js[2].get<uint64_t>();
                        if (f > UINT32_MAX || c > UINT32_MAX || s > UINT32_MAX)
                            continue;
                        subs.push_back({(uint32_t)f, (uint32_t)c, (uint32_t)s});
                    }
                    // Ranges that don't fit the index buffer are dropped here
                    // (and again in the Mesh constructor, defense in depth).
                    m.submeshes = SanitizeSubmeshes(std::move(subs), m.indices.size());
                }
            }
            scene.meshes.push_back(std::move(m));
        }
    }

    if (auto it = root.find("entities"); it != root.end() && it->is_array()) {
        for (const json& je : *it) {
            if (!je.is_object())
                return std::nullopt;
            SavedEntity e;
            e.id = GetOr<uint64_t>(je, "id", 0);
            e.parent = GetOr<uint64_t>(je, "parent", 0);
            e.name = GetOr<std::string>(je, "name", "");
            e.translation = JsonToVec3(je.value("translation", json()), vec3(0.0f));
            e.rotation = JsonToVec3(je.value("rotation", json()), vec3(0.0f));
            e.scale = JsonToVec3(je.value("scale", json()), vec3(1.0f));
            e.meshIndex = GetOr<int>(je, "mesh", -1);
            if (e.meshIndex >= (int)scene.meshes.size())
                return std::nullopt; // dangling mesh reference
            e.albedo = JsonToVec3(je.value("albedo", json()), vec3(0.8f));
            e.metallic = GetOr<float>(je, "metallic", 0.0f);
            e.roughness = GetOr<float>(je, "roughness", 0.5f);
            e.emissive = JsonToVec3(je.value("emissive", json()), vec3(0.0f));
            e.emissiveStrength = GetOr<float>(je, "emissiveStrength", 0.0f);
            e.transmission = GetOr<float>(je, "transmission", 0.0f); // pre-transmission files stay solid
            e.ior = GetOr<float>(je, "ior", 1.5f);
            e.subsurface = GetOr<float>(je, "subsurface", 0.0f); // pre-SSS files stay opaque
            e.subsurfaceColor = JsonToVec3(je.value("subsurfaceColor", json()), vec3(0.9f, 0.8f, 0.7f));
            e.subsurfaceRadius = JsonToVec3(je.value("subsurfaceRadius", json()), vec3(0.1f, 0.05f, 0.03f));
            e.albedoSource = GetOr<std::string>(je, "albedoSource", "");
            e.mrSource = GetOr<std::string>(je, "mrSource", "");
            if (auto mt = je.find("extraMaterials"); mt != je.end() && mt->is_array()) {
                for (const json& jm : *mt) {
                    if (!jm.is_object())
                        continue; // lenient on content
                    SavedMaterial m;
                    m.albedo = JsonToVec3(jm.value("albedo", json()), vec3(0.8f));
                    m.metallic = GetOr<float>(jm, "metallic", 0.0f);
                    m.roughness = GetOr<float>(jm, "roughness", 0.5f);
                    m.emissive = JsonToVec3(jm.value("emissive", json()), vec3(0.0f));
                    m.emissiveStrength = GetOr<float>(jm, "emissiveStrength", 0.0f);
                    m.transmission = GetOr<float>(jm, "transmission", 0.0f);
                    m.ior = GetOr<float>(jm, "ior", 1.5f);
                    m.subsurface = GetOr<float>(jm, "subsurface", 0.0f);
                    m.subsurfaceColor = JsonToVec3(jm.value("subsurfaceColor", json()), vec3(0.9f, 0.8f, 0.7f));
                    m.subsurfaceRadius = JsonToVec3(jm.value("subsurfaceRadius", json()), vec3(0.1f, 0.05f, 0.03f));
                    m.albedoSource = GetOr<std::string>(jm, "albedoSource", "");
                    m.mrSource = GetOr<std::string>(jm, "mrSource", "");
                    e.extraMaterials.push_back(m);
                }
            }
            if (auto lt = je.find("light"); lt != je.end() && lt->is_object()) {
                e.lightEnabled = true;
                e.lightColor = JsonToVec3(lt->value("color", json()), vec3(1.0f));
                e.lightIntensity = GetOr<float>(*lt, "intensity", 0.0f);
                e.lightRange = GetOr<float>(*lt, "range", 0.0f);
            }
            if (auto st = je.find("skeleton"); st != je.end() && st->is_object()) { // v3 (#147)
                SavedSkeleton s;
                if (auto p = st->find("parents"); p != st->end() && p->is_array())
                    for (const json& v : *p)
                        if (v.is_number_integer())
                            s.parents.push_back(v.get<int>());
                if (auto n = st->find("names"); n != st->end() && n->is_array())
                    for (const json& v : *n)
                        s.names.push_back(v.is_string() ? v.get<std::string>() : "");
                if (auto t = st->find("bindT"); t != st->end() && t->is_array())
                    for (const json& v : *t)
                        s.bindT.push_back(JsonToVec3(v, vec3(0.0f)));
                if (auto r = st->find("bindR"); r != st->end() && r->is_array())
                    for (const json& v : *r)
                        s.bindR.push_back(JsonToQuat(v));
                if (auto c = st->find("bindS"); c != st->end() && c->is_array())
                    for (const json& v : *c)
                        s.bindS.push_back(JsonToVec3(v, vec3(1.0f)));
                if (auto ib = st->find("inverseBind"); ib != st->end() && ib->is_array())
                    for (const json& v : *ib)
                        s.inverseBind.push_back(JsonToMat4(v));
                e.skeleton = std::move(s);
            }
            if (auto pt = je.find("pose"); pt != je.end() && pt->is_array())
                for (const json& v : *pt)
                    e.pose.push_back(JsonToQuat(v));
            scene.entities.push_back(std::move(e));
        }
    }

    if (auto it = root.find("extras"); it != root.end())
        scene.extrasJson = it->dump();

    return scene;
}

} // namespace forge
