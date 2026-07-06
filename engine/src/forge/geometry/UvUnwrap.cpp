#include "UvUnwrap.h"

#include "forge/core/Log.h"

#include <xatlas.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace forge {

// xatlas logs through a global printf-style hook; route it into our leveled
// logging once so chart warnings (degenerate faces, failed parameterizations)
// land in the editor console instead of raw stdout.
static int XatlasPrint(const char* format, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (written <= 0)
        return written;
    // xatlas terminates its own lines; FORGE_INFO appends one too.
    size_t len = std::min((size_t)written, sizeof(buffer) - 1);
    while (len > 0 && buffer[len - 1] == '\n')
        buffer[--len] = '\0';
    FORGE_INFO("xatlas: %s", buffer);
    return written;
}

std::optional<UnwrapResult> UnwrapUVData(const std::vector<Vertex>& vertices,
                                         const std::vector<uint32_t>& indices,
                                         const std::vector<Submesh>& submeshes,
                                         const UnwrapOptions& options)
{
    if (vertices.empty() || indices.size() < 3 || indices.size() % 3 != 0)
        return std::nullopt;

    xatlas::SetPrint(XatlasPrint, /*verbose=*/false);

    xatlas::MeshDecl decl;
    decl.vertexCount = (uint32_t)vertices.size();
    decl.vertexPositionData = &vertices[0].position;
    decl.vertexPositionStride = sizeof(Vertex);
    decl.vertexNormalData = &vertices[0].normal;
    decl.vertexNormalStride = sizeof(Vertex);
    decl.indexCount = (uint32_t)indices.size();
    decl.indexData = indices.data();
    decl.indexFormat = xatlas::IndexFormat::UInt32; // default is UInt16

    // Keep charts from spanning material slots: a chart mixing two slots would
    // force both materials onto one texture region when texturing lands (#113).
    std::vector<uint32_t> faceMaterials;
    if (!submeshes.empty()) {
        faceMaterials.assign(indices.size() / 3, 0);
        for (const Submesh& s : submeshes)
            for (uint32_t i = 0; i < s.indexCount; i += 3)
                faceMaterials[(s.firstIndex + i) / 3] = s.materialSlot;
        decl.faceMaterialData = faceMaterials.data();
    }

    xatlas::Atlas* atlas = xatlas::Create();
    xatlas::AddMeshError error = xatlas::AddMesh(atlas, decl, 1);
    if (error != xatlas::AddMeshError::Success) {
        FORGE_ERROR("UnwrapUV: xatlas AddMesh failed: %s", xatlas::StringForEnum(error));
        xatlas::Destroy(atlas);
        return std::nullopt;
    }

    xatlas::ChartOptions chartOptions; // defaults: normal/roundness/straightness weights
    xatlas::PackOptions packOptions;
    packOptions.resolution = options.resolution; // texelsPerUnit 0 -> estimated to fit one page
    packOptions.padding = options.padding;
    packOptions.bruteForce = options.bruteForce;
    xatlas::Generate(atlas, chartOptions, packOptions);

    if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0) {
        FORGE_ERROR("UnwrapUV: no chartable geometry (all faces degenerate?)");
        xatlas::Destroy(atlas);
        return std::nullopt;
    }

    const xatlas::Mesh& out = atlas->meshes[0];
    UnwrapResult result;
    result.vertices.resize(out.vertexCount);
    const float invW = 1.0f / (float)atlas->width;
    const float invH = 1.0f / (float)atlas->height;
    for (uint32_t v = 0; v < out.vertexCount; ++v) {
        const xatlas::Vertex& ov = out.vertexArray[v];
        Vertex nv = vertices[ov.xref]; // position/normal carry over from the source vertex
        // uv is in texel units; a vertex outside every atlas (ignored face) gets (0,0)
        nv.uv = ov.atlasIndex >= 0 ? vec2(ov.uv[0] * invW, ov.uv[1] * invH) : vec2(0.0f);
        result.vertices[v] = nv;
    }
    result.indices.assign(out.indexArray, out.indexArray + out.indexCount);
    // xatlas keeps triangle order, so the input ranges still partition the
    // output index buffer; only the vertex indices inside them changed.
    result.submeshes = submeshes;
    result.chartCount = out.chartCount;
    result.atlasWidth = atlas->width;
    result.atlasHeight = atlas->height;
    result.utilization = atlas->atlasCount > 0 ? atlas->utilization[0] : 0.0f;
    xatlas::Destroy(atlas);

    FORGE_INFO("UnwrapUV: %u charts on a %ux%u atlas, %.0f%% utilization", result.chartCount,
               result.atlasWidth, result.atlasHeight, result.utilization * 100.0f);
    return result;
}

float UvAreaCoverage(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
{
    double area = 0.0;
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() ||
            indices[i + 2] >= vertices.size())
            continue;
        const vec2& a = vertices[indices[i]].uv;
        const vec2& b = vertices[indices[i + 1]].uv;
        const vec2& c = vertices[indices[i + 2]].uv;
        area += 0.5 * std::abs((double)(b.x - a.x) * (c.y - a.y) - (double)(c.x - a.x) * (b.y - a.y));
    }
    return (float)area;
}

} // namespace forge
