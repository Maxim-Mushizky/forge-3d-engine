#include "ModelImporter.h"

#include "forge/core/Log.h"

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define STB_IMAGE_IMPLEMENTATION // single stb implementation for the whole engine
#include <tiny_gltf.h>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <algorithm>
#include <filesystem>
#include <unordered_map>

namespace forge {

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static void ComputeNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
{
    for (Vertex& v : vertices)
        v.normal = vec3(0.0f);
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const vec3& p0 = vertices[indices[i]].position;
        const vec3& p1 = vertices[indices[i + 1]].position;
        const vec3& p2 = vertices[indices[i + 2]].position;
        vec3 n = glm::cross(p1 - p0, p2 - p0);
        vertices[indices[i]].normal += n;
        vertices[indices[i + 1]].normal += n;
        vertices[indices[i + 2]].normal += n;
    }
    for (Vertex& v : vertices) {
        float len = glm::length(v.normal);
        v.normal = len > 1e-8f ? v.normal / len : vec3(0, 1, 0);
    }
}

// ---------------------------------------------------------------------------
// glTF
// ---------------------------------------------------------------------------

static const uint8_t* AccessorData(const tinygltf::Model& model, const tinygltf::Accessor& accessor, size_t& strideOut)
{
    const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[view.buffer];
    int stride = accessor.ByteStride(view);
    strideOut = stride > 0 ? (size_t)stride : 0;
    return buffer.data.data() + view.byteOffset + accessor.byteOffset;
}

static mat4 NodeLocalMatrix(const tinygltf::Node& node)
{
    if (node.matrix.size() == 16) {
        mat4 m;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m[c][r] = (float)node.matrix[c * 4 + r];
        return m;
    }
    mat4 m(1.0f);
    if (node.translation.size() == 3)
        m = glm::translate(m, vec3((float)node.translation[0], (float)node.translation[1], (float)node.translation[2]));
    if (node.rotation.size() == 4)
        m *= glm::mat4_cast(quat((float)node.rotation[3], (float)node.rotation[0], (float)node.rotation[1], (float)node.rotation[2]));
    if (node.scale.size() == 3)
        m = glm::scale(m, vec3((float)node.scale[0], (float)node.scale[1], (float)node.scale[2]));
    return m;
}

static void CollectMeshInstances(const tinygltf::Model& model, int nodeIdx, const mat4& parent,
                                 std::vector<std::pair<int, mat4>>& out)
{
    const tinygltf::Node& node = model.nodes[nodeIdx];
    mat4 world = parent * NodeLocalMatrix(node);
    if (node.mesh >= 0)
        out.push_back({node.mesh, world});
    for (int child : node.children)
        CollectMeshInstances(model, child, world, out);
}

static std::vector<ImportedPart> LoadGLTF(const std::string& path)
{
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string err, warn;

    bool binary = path.size() > 4 && path.substr(path.size() - 4) == ".glb";
    bool ok = binary ? loader.LoadBinaryFromFile(&model, &err, &warn, path)
                     : loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!warn.empty())
        FORGE_WARN("glTF: %s", warn.c_str());
    if (!ok) {
        FORGE_ERROR("glTF load failed (%s): %s", path.c_str(), err.c_str());
        return {};
    }

    // Decoded image -> GL texture, shared between primitives.
    // Cache key includes srgb: the same image used as albedo and as data must not collide.
    std::unordered_map<int64_t, std::shared_ptr<Texture2D>> textureCache;
    auto getTexture = [&](int textureIndex, bool srgb) -> std::shared_ptr<Texture2D> {
        if (textureIndex < 0 || textureIndex >= (int)model.textures.size())
            return nullptr;
        int source = model.textures[textureIndex].source;
        if (source < 0 || source >= (int)model.images.size())
            return nullptr;
        int64_t key = source * 2 + (srgb ? 1 : 0);
        if (auto it = textureCache.find(key); it != textureCache.end())
            return it->second;
        const tinygltf::Image& img = model.images[source];
        if (img.image.empty() || img.bits != 8)
            return nullptr;
        auto tex = std::make_shared<Texture2D>(img.image.data(), (uint32_t)img.width, (uint32_t)img.height,
                                               img.component, srgb);
        textureCache[key] = tex;
        return tex;
    };

    // Which nodes to walk: the default scene, or every node if scenes are absent.
    std::vector<std::pair<int, mat4>> instances;
    if (!model.scenes.empty()) {
        int sceneIdx = model.defaultScene >= 0 ? model.defaultScene : 0;
        for (int root : model.scenes[sceneIdx].nodes)
            CollectMeshInstances(model, root, mat4(1.0f), instances);
    } else {
        for (int m = 0; m < (int)model.meshes.size(); ++m)
            instances.push_back({m, mat4(1.0f)});
    }

    std::vector<ImportedPart> parts;
    for (auto& [meshIdx, world] : instances) {
        const tinygltf::Mesh& gltfMesh = model.meshes[meshIdx];
        mat3 normalMat = mat3(glm::transpose(glm::inverse(world)));

        for (size_t p = 0; p < gltfMesh.primitives.size(); ++p) {
            const tinygltf::Primitive& prim = gltfMesh.primitives[p];
            if (prim.mode != TINYGLTF_MODE_TRIANGLES && prim.mode != -1)
                continue;

            auto posIt = prim.attributes.find("POSITION");
            if (posIt == prim.attributes.end())
                continue;
            const tinygltf::Accessor& posAcc = model.accessors[posIt->second];

            std::vector<Vertex> vertices(posAcc.count);

            size_t stride;
            const uint8_t* data = AccessorData(model, posAcc, stride);
            for (size_t i = 0; i < posAcc.count; ++i) {
                vec3 pos = *(const vec3*)(data + i * stride);
                vertices[i].position = vec3(world * vec4(pos, 1.0f));
            }

            bool hasNormals = false;
            if (auto it = prim.attributes.find("NORMAL"); it != prim.attributes.end()) {
                const tinygltf::Accessor& acc = model.accessors[it->second];
                data = AccessorData(model, acc, stride);
                for (size_t i = 0; i < acc.count && i < vertices.size(); ++i)
                    vertices[i].normal = glm::normalize(normalMat * *(const vec3*)(data + i * stride));
                hasNormals = true;
            }

            if (auto it = prim.attributes.find("TEXCOORD_0"); it != prim.attributes.end()) {
                const tinygltf::Accessor& acc = model.accessors[it->second];
                if (acc.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                    data = AccessorData(model, acc, stride);
                    for (size_t i = 0; i < acc.count && i < vertices.size(); ++i)
                        vertices[i].uv = *(const vec2*)(data + i * stride);
                }
            }

            std::vector<uint32_t> indices;
            if (prim.indices >= 0) {
                const tinygltf::Accessor& acc = model.accessors[prim.indices];
                data = AccessorData(model, acc, stride);
                indices.resize(acc.count);
                for (size_t i = 0; i < acc.count; ++i) {
                    switch (acc.componentType) {
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: indices[i] = *(const uint8_t*)(data + i * stride); break;
                    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: indices[i] = *(const uint16_t*)(data + i * stride); break;
                    default: indices[i] = *(const uint32_t*)(data + i * stride); break;
                    }
                }
            } else {
                indices.resize(vertices.size());
                for (uint32_t i = 0; i < (uint32_t)indices.size(); ++i)
                    indices[i] = i;
            }

            if (!hasNormals)
                ComputeNormals(vertices, indices);

            ImportedPart part;
            part.name = !gltfMesh.name.empty() ? gltfMesh.name : "mesh" + std::to_string(meshIdx);
            if (gltfMesh.primitives.size() > 1)
                part.name += "." + std::to_string(p);

            if (prim.material >= 0) {
                const tinygltf::Material& mat = model.materials[prim.material];
                const auto& pbr = mat.pbrMetallicRoughness;
                part.material.albedo = vec3((float)pbr.baseColorFactor[0], (float)pbr.baseColorFactor[1],
                                            (float)pbr.baseColorFactor[2]);
                part.material.metallic = (float)pbr.metallicFactor;
                part.material.roughness = (float)pbr.roughnessFactor;
                part.material.albedoMap = getTexture(pbr.baseColorTexture.index, /*srgb=*/true);
                part.material.metallicRoughnessMap = getTexture(pbr.metallicRoughnessTexture.index, /*srgb=*/false);
            }

            part.mesh = std::make_shared<Mesh>(std::move(vertices), std::move(indices));
            parts.push_back(std::move(part));
        }
    }
    return parts;
}

// ---------------------------------------------------------------------------
// OBJ
// ---------------------------------------------------------------------------

static std::vector<ImportedPart> LoadOBJ(const std::string& path)
{
    std::string dir = std::filesystem::path(path).parent_path().string();

    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    config.mtl_search_path = dir;

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path, config)) {
        FORGE_ERROR("OBJ load failed (%s): %s", path.c_str(), reader.Error().c_str());
        return {};
    }
    if (!reader.Warning().empty())
        FORGE_WARN("OBJ: %s", reader.Warning().c_str());

    const tinyobj::attrib_t& attrib = reader.GetAttrib();
    const auto& materials = reader.GetMaterials();

    std::unordered_map<std::string, std::shared_ptr<Texture2D>> textureCache;
    auto getTexture = [&](const std::string& name) -> std::shared_ptr<Texture2D> {
        if (name.empty())
            return nullptr;
        if (auto it = textureCache.find(name); it != textureCache.end())
            return it->second;
        std::string full = (std::filesystem::path(dir) / name).string();
        auto tex = Texture2D::FromFile(full, /*srgb=*/true, /*flipV=*/true); // OBJ: v=0 at bottom
        textureCache[name] = tex;
        return tex;
    };

    std::vector<ImportedPart> parts;
    for (const tinyobj::shape_t& shape : reader.GetShapes()) {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve(shape.mesh.indices.size());
        bool missingNormals = false;

        for (const tinyobj::index_t& idx : shape.mesh.indices) {
            Vertex v{};
            v.position = {attrib.vertices[3 * idx.vertex_index],
                          attrib.vertices[3 * idx.vertex_index + 1],
                          attrib.vertices[3 * idx.vertex_index + 2]};
            if (idx.normal_index >= 0)
                v.normal = {attrib.normals[3 * idx.normal_index],
                            attrib.normals[3 * idx.normal_index + 1],
                            attrib.normals[3 * idx.normal_index + 2]};
            else
                missingNormals = true;
            if (idx.texcoord_index >= 0)
                v.uv = {attrib.texcoords[2 * idx.texcoord_index],
                        attrib.texcoords[2 * idx.texcoord_index + 1]};
            indices.push_back((uint32_t)vertices.size());
            vertices.push_back(v);
        }

        if (vertices.empty())
            continue;
        if (missingNormals)
            ComputeNormals(vertices, indices);

        ImportedPart part;
        part.name = shape.name.empty() ? "shape" : shape.name;
        int matId = shape.mesh.material_ids.empty() ? -1 : shape.mesh.material_ids[0];
        if (matId >= 0 && matId < (int)materials.size()) {
            const tinyobj::material_t& m = materials[matId];
            part.material.albedo = {m.diffuse[0], m.diffuse[1], m.diffuse[2]};
            part.material.albedoMap = getTexture(m.diffuse_texname);
            part.material.metallic = 0.0f;
            // Phong shininess -> rough approximation of roughness.
            part.material.roughness = m.shininess > 0.0f
                ? glm::clamp(std::sqrt(2.0f / (m.shininess + 2.0f)), 0.05f, 1.0f)
                : 0.7f;
        }
        part.mesh = std::make_shared<Mesh>(std::move(vertices), std::move(indices));
        parts.push_back(std::move(part));
    }
    return parts;
}

// ---------------------------------------------------------------------------

std::vector<ImportedPart> ModelImporter::Load(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });

    std::vector<ImportedPart> parts;
    if (ext == ".gltf" || ext == ".glb")
        parts = LoadGLTF(path);
    else if (ext == ".obj")
        parts = LoadOBJ(path);
    else
        FORGE_ERROR("Unsupported model format: %s", ext.c_str());

    size_t tris = 0;
    for (const auto& p : parts)
        tris += p.mesh->Indices().size() / 3;
    if (!parts.empty())
        FORGE_INFO("Imported %s: %zu part(s), %zu triangles", path.c_str(), parts.size(), tris);
    return parts;
}

} // namespace forge
