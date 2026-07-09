#include "ModelImporter.h"

#include "forge/anim/Morph.h"
#include "forge/anim/SkinImport.h"
#include "forge/core/Log.h"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define STB_IMAGE_IMPLEMENTATION // single stb implementation for the whole engine
#include <tiny_gltf.h>

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <algorithm>
#include <cmath>
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
    strideOut = 0;
    // Spec-legal accessors may omit bufferView entirely (sparse / zero-filled
    // forms): there is nothing to read — callers skip the attribute instead of
    // indexing bufferViews[-1] on external data.
    if (accessor.bufferView < 0 || accessor.bufferView >= (int)model.bufferViews.size())
        return nullptr;
    const tinygltf::BufferView& view = model.bufferViews[accessor.bufferView];
    if (view.buffer < 0 || view.buffer >= (int)model.buffers.size())
        return nullptr;
    const tinygltf::Buffer& buffer = model.buffers[view.buffer];
    int stride = accessor.ByteStride(view);
    strideOut = stride > 0 ? (size_t)stride : 0;
    return buffer.data.data() + view.byteOffset + accessor.byteOffset;
}

// Bounds-checked pointer into a buffer through a bufferView at byteOffset, valid
// for bytesNeeded bytes. Sparse index/value blocks read through this rather than
// AccessorData: they reference bufferViews directly, not via an accessor.
static const uint8_t* BufferViewData(const tinygltf::Model& model, int viewIdx, size_t byteOffset,
                                     size_t bytesNeeded)
{
    if (viewIdx < 0 || viewIdx >= (int)model.bufferViews.size())
        return nullptr;
    const tinygltf::BufferView& view = model.bufferViews[viewIdx];
    if (view.buffer < 0 || view.buffer >= (int)model.buffers.size())
        return nullptr;
    const tinygltf::Buffer& buffer = model.buffers[view.buffer];
    if (view.byteOffset > buffer.data.size() ||
        view.byteLength > buffer.data.size() - view.byteOffset)
        return nullptr;
    if (byteOffset > view.byteLength || bytesNeeded > view.byteLength - byteOffset)
        return nullptr;
    return buffer.data.data() + view.byteOffset + byteOffset;
}

// Morph-target delta accessor (VEC3 float) -> per-vertex vec3s (#149). Dense data
// comes from the accessor's bufferView; an ABSENT bufferView means a zero-filled
// base (spec-legal, THE common encoding for sparse face shapes); the sparse
// overlay then replaces the flagged elements. False = unreadable or spec-
// violating — the caller drops the mesh's morphs rather than deform with garbage.
static bool ReadMorphDeltas(const tinygltf::Model& model, int accessorIdx, size_t vertexCount,
                            std::vector<vec3>& out)
{
    if (accessorIdx < 0 || accessorIdx >= (int)model.accessors.size())
        return false;
    const tinygltf::Accessor& acc = model.accessors[accessorIdx];
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || acc.type != TINYGLTF_TYPE_VEC3)
        return false;
    if (acc.count != vertexCount)
        return false; // deltas must parallel the primitive's vertices
    out.assign(vertexCount, vec3(0.0f));
    size_t stride;
    if (const uint8_t* data = AccessorData(model, acc, stride))
        for (size_t i = 0; i < vertexCount; ++i)
            out[i] = *(const vec3*)(data + i * stride);
    if (!acc.sparse.isSparse)
        return true;
    const auto& sparse = acc.sparse;
    if (sparse.count < 1 || (size_t)sparse.count > vertexCount)
        return false; // schema: 1 <= sparse.count <= accessor.count
    size_t indexSize;
    switch (sparse.indices.componentType) {
    case kGltfUnsignedByte: indexSize = 1; break;
    case kGltfUnsignedShort: indexSize = 2; break;
    case kGltfUnsignedInt: indexSize = 4; break;
    default:
        return false; // spec: u8/u16/u32 only (5124 SIGNED int is explicitly illegal)
    }
    const uint8_t* indices = BufferViewData(model, sparse.indices.bufferView,
                                            sparse.indices.byteOffset,
                                            (size_t)sparse.count * indexSize);
    const uint8_t* values =
        BufferViewData(model, sparse.values.bufferView, sparse.values.byteOffset,
                       (size_t)sparse.count * sizeof(vec3));
    return ApplySparseOverlay(out, indices, sparse.indices.componentType, values,
                              (size_t)sparse.count);
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

// A mesh reference found in the node tree. The node index rides along because
// skinning (#146) is a NODE property (`node.skin`), lost if we only keep the
// accumulated matrix.
struct MeshInstance {
    int mesh = -1;
    int node = -1; // -1 = no source node (scene-less fallback), always rigid
    mat4 world{1.0f};
};

static void CollectMeshInstances(const tinygltf::Model& model, int nodeIdx, const mat4& parent,
                                 std::vector<MeshInstance>& out)
{
    const tinygltf::Node& node = model.nodes[nodeIdx];
    mat4 world = parent * NodeLocalMatrix(node);
    if (node.mesh >= 0)
        out.push_back({node.mesh, nodeIdx, world});
    for (int child : node.children)
        CollectMeshInstances(model, child, world, out);
}

// Best-effort TRS from an arbitrary matrix: glm::decompose fails on degenerate
// (singular) matrices; fall back to column extraction so one bad node never
// aborts an import.
static void DecomposeTRS(const mat4& m, vec3& t, quat& r, vec3& s)
{
    vec3 skew;
    vec4 perspective;
    if (glm::decompose(m, s, r, t, skew, perspective)) {
        // Folded chains (non-uniform parent scale over a rotated joint) can
        // shear, which TRS-only joint storage cannot represent — approximate,
        // but never silently.
        if (glm::length(skew) > 1e-4f)
            FORGE_WARN("glTF: sheared joint chain — shear is unrepresentable in TRS, bind approximated");
        return;
    }
    FORGE_WARN("glTF: joint matrix decompose failed (shear/degenerate) — best-effort TRS");
    t = vec3(m[3]);
    s = vec3(glm::length(vec3(m[0])), glm::length(vec3(m[1])), glm::length(vec3(m[2])));
    if (s.x > 1e-8f && s.y > 1e-8f && s.z > 1e-8f)
        r = glm::quat_cast(mat3(vec3(m[0]) / s.x, vec3(m[1]) / s.y, vec3(m[2]) / s.z));
    else
        r = quat(1.0f, 0.0f, 0.0f, 0.0f);
}

// Local bind TRS of a joint node. glTF stores rotation as [x,y,z,w] while
// glm::quat's ctor is (w,x,y,z) — same reorder as NodeLocalMatrix above.
// Matrix-form nodes decompose.
static void NodeLocalTRS(const tinygltf::Node& node, vec3& t, quat& r, vec3& s)
{
    t = vec3(0.0f);
    r = quat(1.0f, 0.0f, 0.0f, 0.0f);
    s = vec3(1.0f);
    if (node.matrix.size() == 16) {
        DecomposeTRS(NodeLocalMatrix(node), t, r, s);
        return;
    }
    if (node.translation.size() == 3)
        t = vec3((float)node.translation[0], (float)node.translation[1], (float)node.translation[2]);
    if (node.rotation.size() == 4)
        r = quat((float)node.rotation[3], (float)node.rotation[0], (float)node.rotation[1],
                 (float)node.rotation[2]);
    if (node.scale.size() == 3)
        s = vec3((float)node.scale[0], (float)node.scale[1], (float)node.scale[2]);
}

// Builds the shared Skeleton for model.skins[skinIdx]. Joint parents derive
// from the node tree — nearest ancestor that is itself a joint of this skin;
// skin.skeleton is only a hint and is not trusted structurally. Transforms of
// non-joint ancestors fold into root joint locals: without the fold, palettes
// are wrong whenever the rig sits under a transformed group node (CesiumMan-
// style assets). Joints come out topo-sorted (parents[i] < i); remapOut is the
// old->new joint remap the caller must apply to every vertex's joint indices.
static std::shared_ptr<Skeleton> BuildSkeleton(const tinygltf::Model& model, int skinIdx,
                                               std::vector<int>& remapOut)
{
    const tinygltf::Skin& skin = model.skins[skinIdx];
    const int jointCount = (int)skin.joints.size();
    const int nodeCount = (int)model.nodes.size();

    // Node -> parent, from the children lists (glTF stores only downward links).
    std::vector<int> nodeParent(nodeCount, -1);
    for (int n = 0; n < nodeCount; ++n)
        for (int child : model.nodes[n].children)
            if (child >= 0 && child < nodeCount)
                nodeParent[child] = n;

    std::unordered_map<int, int> jointOfNode; // node index -> old joint index
    for (int j = 0; j < jointCount; ++j)
        if (skin.joints[j] >= 0 && skin.joints[j] < nodeCount)
            jointOfNode[skin.joints[j]] = j;

    std::vector<int> parents(jointCount, -1);
    std::vector<std::string> names(jointCount);
    std::vector<vec3> bindT(jointCount, vec3(0.0f));
    std::vector<quat> bindR(jointCount, quat(1.0f, 0.0f, 0.0f, 0.0f));
    std::vector<vec3> bindS(jointCount, vec3(1.0f));
    std::vector<mat4> ibms(jointCount, mat4(1.0f));

    for (int j = 0; j < jointCount; ++j) {
        const int nodeIdx = skin.joints[j];
        if (nodeIdx < 0 || nodeIdx >= nodeCount) {
            FORGE_WARN("glTF: skin %d joint %d references invalid node %d — identity joint",
                       skinIdx, j, nodeIdx);
            continue;
        }
        const tinygltf::Node& node = model.nodes[nodeIdx];
        names[j] = node.name;
        NodeLocalTRS(node, bindT[j], bindR[j], bindS[j]);

        // Nearest ancestor that is also a joint. Non-joint nodes on the way up
        // (helper/constraint nodes, joints pruned from skin.joints by
        // optimizers — all spec-legal) are collected so their transforms fold
        // into this joint's local bind: per glTF a joint's global is its NODE
        // global, so dropping the in-between locals would misplace every
        // vertex weighted below them. The walk is capped so a hostile
        // node-parent cycle can't spin forever.
        std::vector<int> skipped; // bottom-up, multiplied top-down below
        int ancestor = nodeParent[nodeIdx];
        for (int guard = 0; ancestor >= 0 && guard < nodeCount; ++guard) {
            if (auto it = jointOfNode.find(ancestor); it != jointOfNode.end()) {
                parents[j] = it->second;
                break;
            }
            skipped.push_back(ancestor);
            ancestor = nodeParent[ancestor];
        }

        // Fold the skipped chain (root joints: everything up to the scene
        // root) so joint globals place the rig exactly where the file's node
        // hierarchy does.
        if (!skipped.empty()) {
            mat4 chain(1.0f);
            for (auto it = skipped.rbegin(); it != skipped.rend(); ++it)
                chain = chain * NodeLocalMatrix(model.nodes[*it]);
            if (chain != mat4(1.0f)) { // identity chain: keep the authored TRS bit-exact
                mat4 local = glm::translate(mat4(1.0f), bindT[j]) * glm::mat4_cast(bindR[j]) *
                             glm::scale(mat4(1.0f), bindS[j]);
                DecomposeTRS(chain * local, bindT[j], bindR[j], bindS[j]);
            }
        }
    }

    // IBMs: mat4 float accessor, column-major like node.matrix. Absent is
    // spec-legal and means identity.
    if (skin.inverseBindMatrices >= 0 && skin.inverseBindMatrices < (int)model.accessors.size()) {
        const tinygltf::Accessor& acc = model.accessors[skin.inverseBindMatrices];
        size_t stride;
        const uint8_t* data = AccessorData(model, acc, stride);
        if (acc.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && acc.type == TINYGLTF_TYPE_MAT4 &&
            data) {
            for (int j = 0; j < jointCount && j < (int)acc.count; ++j) {
                const float* f = (const float*)(data + (size_t)j * stride);
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        ibms[j][c][r] = f[c * 4 + r];
            }
            if ((int)acc.count < jointCount)
                FORGE_WARN("glTF: skin %d has %d inverse-bind matrices for %d joints — rest identity",
                           skinIdx, (int)acc.count, jointCount);
        } else {
            FORGE_WARN("glTF: skin %d inverseBindMatrices is not a readable float mat4 accessor "
                       "— identity",
                       skinIdx);
        }
    }

    // Topo sort + one consistent remap across every per-joint attribute AND
    // (by the caller) the per-vertex joint indices.
    JointOrder order = TopoSortJoints(parents);
    auto skeleton = std::make_shared<Skeleton>();
    skeleton->parents = order.sortedParents;
    skeleton->names = ReorderJoints(names, order.order);
    skeleton->bindT = ReorderJoints(bindT, order.order);
    skeleton->bindR = ReorderJoints(bindR, order.order);
    skeleton->bindS = ReorderJoints(bindS, order.order);
    skeleton->inverseBind = ReorderJoints(ibms, order.order);
    remapOut = std::move(order.remap);
    return skeleton;
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
    std::vector<MeshInstance> instances;
    if (!model.scenes.empty()) {
        int sceneIdx = model.defaultScene >= 0 ? model.defaultScene : 0;
        for (int root : model.scenes[sceneIdx].nodes)
            CollectMeshInstances(model, root, mat4(1.0f), instances);
    } else {
        for (int m = 0; m < (int)model.meshes.size(); ++m)
            instances.push_back({m, -1, mat4(1.0f)});
    }

    // One Skeleton per glTF skin, shared by every part referencing it. The
    // topo-sort remap must also hit each part's vertex joint indices, so it is
    // cached alongside.
    struct BuiltSkin {
        std::shared_ptr<Skeleton> skeleton;
        std::vector<int> remap; // old joint index -> topo-sorted index
    };
    std::unordered_map<int, BuiltSkin> skinCache;
    bool warnedBadJointIndex = false;
    bool warnedBadWeight = false;

    std::vector<ImportedPart> parts;
    for (const MeshInstance& inst : instances) {
        const int meshIdx = inst.mesh;
        const tinygltf::Mesh& gltfMesh = model.meshes[meshIdx];

        // Skinned node (#146): per glTF 2.0 the node's own transform chain is
        // IGNORED for skinned meshes — joint globals supply all placement — so
        // baking `world` here would double-transform. Vertices are read raw;
        // rigid nodes keep the existing bake path untouched.
        const int skinIdx = inst.node >= 0 ? model.nodes[inst.node].skin : -1;
        bool skinned = skinIdx >= 0;
        if (skinned && (skinIdx >= (int)model.skins.size() || model.skins[skinIdx].joints.empty())) {
            FORGE_WARN("glTF: node references invalid/empty skin %d — imported rigid", skinIdx);
            skinned = false;
        }
        const mat4 world = skinned ? mat4(1.0f) : inst.world;
        mat3 normalMat = mat3(glm::transpose(glm::inverse(world)));

        // All primitives of one glTF mesh merge into ONE part (#80): shared
        // vertex/index buffers plus a submesh range per primitive. Primitives
        // sharing a glTF material share a slot.
        std::vector<Vertex> meshVertices;
        std::vector<uint32_t> meshIndices;
        std::vector<Submesh> submeshes;
        std::vector<VertexSkin> meshSkin;                    // parallel to meshVertices when skinned
        std::vector<MorphTarget> meshMorphs;                 // merged morph targets (#149)
        int expectedTargets = -1;                            // -1 = no primitive processed yet
        bool morphsValid = true;
        std::vector<Material> slotMaterials;                 // slot -> material
        std::unordered_map<int, uint32_t> slotOfGltfMaterial; // glTF material index (-1 = none) -> slot

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
            if (!data) {
                FORGE_WARN("glTF: POSITION accessor has no buffer view in %s — primitive skipped",
                           path.c_str());
                continue;
            }
            for (size_t i = 0; i < posAcc.count; ++i) {
                vec3 pos = *(const vec3*)(data + i * stride);
                vertices[i].position = vec3(world * vec4(pos, 1.0f));
            }

            bool hasNormals = false;
            if (auto it = prim.attributes.find("NORMAL"); it != prim.attributes.end()) {
                const tinygltf::Accessor& acc = model.accessors[it->second];
                data = AccessorData(model, acc, stride);
                for (size_t i = 0; data && i < acc.count && i < vertices.size(); ++i)
                    vertices[i].normal = glm::normalize(normalMat * *(const vec3*)(data + i * stride));
                hasNormals = data != nullptr;
            }

            if (auto it = prim.attributes.find("TEXCOORD_0"); it != prim.attributes.end()) {
                const tinygltf::Accessor& acc = model.accessors[it->second];
                if (acc.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT) {
                    data = AccessorData(model, acc, stride);
                    for (size_t i = 0; data && i < acc.count && i < vertices.size(); ++i)
                        vertices[i].uv = *(const vec2*)(data + i * stride);
                }
            }

            // JOINTS_0/WEIGHTS_0 read like POSITION and merged with the same
            // vertexBase offsets. A primitive missing them (invalid but seen in
            // the wild) leaves weight-0 entries the skinning kernel passes
            // through at bind pose.
            std::vector<VertexSkin> primSkin;
            if (skinned) {
                primSkin.resize(vertices.size());
                auto jIt = prim.attributes.find("JOINTS_0");
                auto wIt = prim.attributes.find("WEIGHTS_0");
                if (jIt != prim.attributes.end() && wIt != prim.attributes.end()) {
                    const tinygltf::Accessor& jAcc = model.accessors[jIt->second];
                    const tinygltf::Accessor& wAcc = model.accessors[wIt->second];
                    const bool knownTypes =
                        (jAcc.componentType == kGltfUnsignedByte ||
                         jAcc.componentType == kGltfUnsignedShort ||
                         jAcc.componentType == kGltfUnsignedInt) &&
                        (wAcc.componentType == kGltfFloat ||
                         wAcc.componentType == kGltfUnsignedByte ||
                         wAcc.componentType == kGltfUnsignedShort);
                    if (!knownTypes)
                        FORGE_WARN("glTF: unsupported JOINTS_0/WEIGHTS_0 component type in %s — "
                                   "primitive imports unweighted",
                                   path.c_str());
                    size_t jStride, wStride;
                    const uint8_t* jData = AccessorData(model, jAcc, jStride);
                    const uint8_t* wData = AccessorData(model, wAcc, wStride);
                    const size_t jointsInSkin = model.skins[skinIdx].joints.size();
                    for (size_t i = 0; knownTypes && jData && wData &&
                                       i < vertices.size() && i < jAcc.count && i < wAcc.count;
                         ++i) {
                        VertexSkin vs;
                        vs.joints = DecodeJointIndices(jData + i * jStride, jAcc.componentType);
                        vs.weights = DecodeWeights(wData + i * wStride, wAcc.componentType);
                        for (int c = 0; c < 4; ++c) {
                            // External data: both recoverable — drop the influence
                            // (remaining weights renormalize below), keep going.
                            if (!(vs.weights[c] >= 0.0f) || !std::isfinite(vs.weights[c])) {
                                // !(w >= 0) is deliberate: it also catches NaN.
                                if (!warnedBadWeight) {
                                    FORGE_WARN("glTF: NaN/negative skin weight in %s — dropped",
                                               path.c_str());
                                    warnedBadWeight = true;
                                }
                                vs.weights[c] = 0.0f;
                            }
                            if (vs.joints[c] >= jointsInSkin) {
                                if (vs.weights[c] != 0.0f && !warnedBadJointIndex) {
                                    FORGE_WARN("glTF: joint index out of range in %s — influence dropped",
                                               path.c_str());
                                    warnedBadJointIndex = true;
                                }
                                vs.joints[c] = 0;
                                vs.weights[c] = 0.0f;
                            }
                        }
                        vs.weights = RenormalizeWeights(vs.weights);
                        primSkin[i] = vs;
                    }
                }
            }

            std::vector<uint32_t> indices;
            if (prim.indices >= 0) {
                const tinygltf::Accessor& acc = model.accessors[prim.indices];
                data = AccessorData(model, acc, stride);
                indices.resize(data ? acc.count : 0);
                for (size_t i = 0; i < indices.size(); ++i) {
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

            // Per primitive, before merging: computing over the merged buffers
            // would clobber good normals from primitives that have them.
            if (!hasNormals)
                ComputeNormals(vertices, indices);

            uint32_t slot;
            if (auto it = slotOfGltfMaterial.find(prim.material); it != slotOfGltfMaterial.end()) {
                slot = it->second;
            } else {
                slot = (uint32_t)slotMaterials.size();
                slotOfGltfMaterial[prim.material] = slot;
                Material m; // primitives without a glTF material get the defaults
                if (prim.material >= 0) {
                    const tinygltf::Material& mat = model.materials[prim.material];
                    const auto& pbr = mat.pbrMetallicRoughness;
                    m.albedo = vec3((float)pbr.baseColorFactor[0], (float)pbr.baseColorFactor[1],
                                    (float)pbr.baseColorFactor[2]);
                    m.metallic = (float)pbr.metallicFactor;
                    m.roughness = (float)pbr.roughnessFactor;
                    m.albedoMap = getTexture(pbr.baseColorTexture.index, /*srgb=*/true);
                    m.metallicRoughnessMap = getTexture(pbr.metallicRoughnessTexture.index, /*srgb=*/false);
                }
                slotMaterials.push_back(std::move(m));
            }

            // The merged buffers use 32-bit offsets; a model that would overflow
            // them is beyond anything drawable anyway — skip the primitive.
            if (meshVertices.size() + vertices.size() > UINT32_MAX ||
                meshIndices.size() + indices.size() > UINT32_MAX) {
                FORGE_WARN("glTF: primitive skipped, merged mesh exceeds 32-bit vertex/index range");
                continue;
            }
            const uint32_t vertexBase = (uint32_t)meshVertices.size();
            const uint32_t firstIndex = (uint32_t)meshIndices.size();
            meshVertices.insert(meshVertices.end(), vertices.begin(), vertices.end());
            if (skinned)
                meshSkin.insert(meshSkin.end(), primSkin.begin(), primSkin.end());
            meshIndices.reserve(meshIndices.size() + indices.size());
            for (uint32_t i : indices)
                meshIndices.push_back(vertexBase + i);
            submeshes.push_back({firstIndex, (uint32_t)indices.size(), slot});

            // Morph targets (#149), merged with the same vertexBase offsets. glTF
            // requires every primitive of a mesh to carry the SAME target count —
            // on mismatch drop the mesh's morphs, keep the geometry. A failed
            // target read drops them too: a partial set would desync name->index
            // resolution against the file's targetNames.
            if (morphsValid) {
                const int targetCount = (int)prim.targets.size();
                if (expectedTargets < 0) {
                    expectedTargets = targetCount;
                    meshMorphs.resize((size_t)targetCount);
                } else if (targetCount != expectedTargets) {
                    FORGE_WARN("glTF: primitives of mesh \"%s\" disagree on morph-target count "
                               "(%d vs %d) — morphs dropped",
                               gltfMesh.name.c_str(), targetCount, expectedTargets);
                    morphsValid = false;
                    meshMorphs.clear();
                }
                for (int t = 0; morphsValid && t < targetCount; ++t) {
                    MorphTarget& mt = meshMorphs[(size_t)t];
                    // POSITION absent = zero displacement (spec-legal: a target may
                    // displace normals only). Deltas read raw here; rigid nodes bake
                    // the world transform in at attach time (TransformMorphDeltas
                    // below) to match the world-baked base vertices.
                    std::vector<vec3> deltas(vertices.size(), vec3(0.0f));
                    if (auto dIt = prim.targets[t].find("POSITION");
                        dIt != prim.targets[t].end() &&
                        !ReadMorphDeltas(model, dIt->second, vertices.size(), deltas)) {
                        FORGE_WARN("glTF: unreadable morph POSITION deltas (target %d, mesh "
                                   "\"%s\") — morphs dropped",
                                   t, gltfMesh.name.c_str());
                        morphsValid = false;
                        meshMorphs.clear();
                        break;
                    }
                    mt.positionDeltas.insert(mt.positionDeltas.end(), deltas.begin(),
                                             deltas.end());
                    if (auto nIt = prim.targets[t].find("NORMAL");
                        nIt != prim.targets[t].end()) {
                        // Present-but-unreadable is corrupt, not absent: drop the
                        // morphs like the POSITION path rather than silently losing
                        // this target's normal displacement.
                        if (!ReadMorphDeltas(model, nIt->second, vertices.size(), deltas)) {
                            FORGE_WARN("glTF: unreadable morph NORMAL deltas (target %d, mesh "
                                       "\"%s\") — morphs dropped",
                                       t, gltfMesh.name.c_str());
                            morphsValid = false;
                            meshMorphs.clear();
                            break;
                        }
                        // Zero-fill the ranges of earlier primitives that lacked
                        // normal deltas so the optional array stays parallel.
                        mt.normalDeltas.resize(vertexBase, vec3(0.0f));
                        mt.normalDeltas.insert(mt.normalDeltas.end(), deltas.begin(),
                                               deltas.end());
                    }
                }
            }
        }

        if (meshVertices.empty() || meshIndices.empty())
            continue;

        ImportedPart part;
        part.name = !gltfMesh.name.empty() ? gltfMesh.name : "mesh" + std::to_string(meshIdx);
        if (!slotMaterials.empty()) {
            part.material = slotMaterials.front();
            part.extraMaterials.assign(slotMaterials.begin() + 1, slotMaterials.end());
        }
        // Single-slot meshes stay submesh-free: the whole-buffer fast path.
        if (slotMaterials.size() <= 1)
            submeshes.clear();
        part.mesh = std::make_shared<Mesh>(std::move(meshVertices), std::move(meshIndices), std::move(submeshes));
        if (skinned) {
            auto it = skinCache.find(skinIdx);
            if (it == skinCache.end()) {
                BuiltSkin built;
                built.skeleton = BuildSkeleton(model, skinIdx, built.remap);
                it = skinCache.emplace(skinIdx, std::move(built)).first;
            }
            part.skeleton = it->second.skeleton;
            RemapVertexJoints(meshSkin, it->second.remap);
            // The importer attaches skin data to the cached mesh: that original
            // never deforms (the editor clones skinned meshes before posing),
            // so its bind snapshot stays valid for every re-import of the path.
            part.mesh->SetSkin(std::move(meshSkin));
        }
        if (morphsValid && !meshMorphs.empty()) {
            // Primitives without NORMAL deltas leave the optional array short —
            // pad to fully parallel (empty stays empty; Mesh validates on attach).
            const size_t vertexCount = part.mesh->Vertices().size();
            for (MorphTarget& mt : meshMorphs)
                if (!mt.normalDeltas.empty())
                    mt.normalDeltas.resize(vertexCount, vec3(0.0f));
            // Rigid nodes bake `world` into the base vertices (and normalMat into
            // the normals), so the deltas must ride the same transform — otherwise
            // ApplyDeform sums a model-space delta onto a world-baked base and the
            // morph displaces in the wrong direction/scale. Skinned meshes read
            // vertices raw (world forced identity above), so they skip this.
            if (!skinned)
                TransformMorphDeltas(meshMorphs, mat3(world), normalMat);
            // Names ride on mesh.extras.targetNames (Blender/three.js convention,
            // not spec) — ARKit-style face rigs resolve set_expression through them.
            for (size_t t = 0; t < meshMorphs.size(); ++t) {
                meshMorphs[t].name = "morphTarget" + std::to_string(t);
                if (gltfMesh.extras.IsObject() && gltfMesh.extras.Has("targetNames")) {
                    const tinygltf::Value& names = gltfMesh.extras.Get("targetNames");
                    if (names.IsArray() && t < names.ArrayLen() && names.Get(t).IsString())
                        meshMorphs[t].name = names.Get(t).Get<std::string>();
                }
            }
            // Initial weights precedence (spec normative): node.weights over
            // mesh.weights, both absent = zeros. Non-finite file values zero out
            // — a NaN weight would poison every morphed vertex downstream.
            part.defaultMorphWeights.assign(meshMorphs.size(), 0.0f);
            const std::vector<double>& weights =
                (inst.node >= 0 && !model.nodes[inst.node].weights.empty())
                    ? model.nodes[inst.node].weights
                    : gltfMesh.weights;
            for (size_t i = 0; i < part.defaultMorphWeights.size() && i < weights.size(); ++i)
                if (std::isfinite(weights[i]))
                    part.defaultMorphWeights[i] = (float)weights[i];
            // Cached-mesh discipline as for the skin above: the original never
            // deforms, so its bind snapshot stays valid across re-imports.
            part.mesh->SetMorphTargets(std::move(meshMorphs));
        }
        parts.push_back(std::move(part));
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
