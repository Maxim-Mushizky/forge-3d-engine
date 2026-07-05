#include "MeshFactory.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cfloat>

namespace forge {

std::shared_ptr<Mesh> MeshFactory::Cube()
{
    // 24 vertices: each face has its own normal.
    static const vec3 faceNormals[6] = {
        {0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0},
    };

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve(24);
    indices.reserve(36);

    for (uint32_t f = 0; f < 6; ++f) {
        vec3 n = faceNormals[f];
        // Build a tangent basis for the face.
        vec3 u = (std::abs(n.y) > 0.9f) ? vec3(1, 0, 0) : glm::normalize(glm::cross(vec3(0, 1, 0), n));
        vec3 v = glm::cross(n, u);

        uint32_t base = (uint32_t)vertices.size();
        const vec2 corners[4] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
        for (int c = 0; c < 4; ++c) {
            vec3 pos = n * 0.5f + u * corners[c].x + v * corners[c].y;
            vertices.push_back({pos, n, corners[c] + vec2(0.5f)});
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }

    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

std::shared_ptr<Mesh> MeshFactory::Sphere(uint32_t rings, uint32_t sectors)
{
    // Lower bounds: below these the pole-row emission yields no triangles / a
    // flat sliver. Upper bounds: the MCP spawn params arrive as json ints, so
    // a negative value wraps to ~4e9 here and an unbounded loop would OOM the
    // exception-light editor.
    rings = std::clamp(rings, 2u, 512u);
    sectors = std::clamp(sectors, 3u, 1024u);

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve((rings + 1) * (sectors + 1));

    const float pi = glm::pi<float>();
    for (uint32_t r = 0; r <= rings; ++r) {
        float phi = pi * (float)r / (float)rings; // 0..pi from north pole
        for (uint32_t s = 0; s <= sectors; ++s) {
            float theta = 2.0f * pi * (float)s / (float)sectors;
            vec3 n{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            vertices.push_back({n * 0.5f, n, {(float)s / sectors, 1.0f - (float)r / rings}});
        }
    }

    // Counter-clockwise seen from outside, like every other primitive: winding
    // is what RecomputeNormalsWelded derives normals from after any edit, so a
    // backwards sphere shades dark and inflates inward the moment it is
    // sculpted (#116). The pole rows collapse one quad edge (both r=0 verts
    // share the pole position), so emit only the non-degenerate half there —
    // zero-area slivers would pollute the welded normal accumulation at the
    // poles.
    for (uint32_t r = 0; r < rings; ++r) {
        for (uint32_t s = 0; s < sectors; ++s) {
            uint32_t i0 = r * (sectors + 1) + s;
            uint32_t i1 = i0 + sectors + 1;
            if (r > 0)
                indices.insert(indices.end(), {i0, i0 + 1, i1});
            if (r + 1 < rings)
                indices.insert(indices.end(), {i0 + 1, i1 + 1, i1});
        }
    }

    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

std::shared_ptr<Mesh> MeshFactory::Cylinder(uint32_t sectors)
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    const float pi = glm::pi<float>();
    const float r = 0.5f, h = 0.5f;

    // Side wall.
    for (uint32_t s = 0; s <= sectors; ++s) {
        float theta = 2.0f * pi * (float)s / (float)sectors;
        vec3 n{std::cos(theta), 0.0f, std::sin(theta)};
        float u = (float)s / sectors;
        vertices.push_back({{n.x * r, h, n.z * r}, n, {u, 1}});
        vertices.push_back({{n.x * r, -h, n.z * r}, n, {u, 0}});
    }
    for (uint32_t s = 0; s < sectors; ++s) {
        uint32_t i = s * 2;
        indices.insert(indices.end(), {i, i + 2, i + 1, i + 1, i + 2, i + 3});
    }

    // Caps (fan around a center vertex).
    for (int cap = 0; cap < 2; ++cap) {
        float y = cap == 0 ? h : -h;
        vec3 n{0.0f, cap == 0 ? 1.0f : -1.0f, 0.0f};
        uint32_t center = (uint32_t)vertices.size();
        vertices.push_back({{0, y, 0}, n, {0.5f, 0.5f}});
        for (uint32_t s = 0; s <= sectors; ++s) {
            float theta = 2.0f * pi * (float)s / (float)sectors;
            vertices.push_back({{std::cos(theta) * r, y, std::sin(theta) * r}, n,
                                {0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta)}});
        }
        for (uint32_t s = 0; s < sectors; ++s) {
            if (cap == 0)
                indices.insert(indices.end(), {center, center + 2 + s, center + 1 + s});
            else
                indices.insert(indices.end(), {center, center + 1 + s, center + 2 + s});
        }
    }

    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

std::shared_ptr<Mesh> MeshFactory::Cone(uint32_t sectors)
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    const float pi = glm::pi<float>();
    const float r = 0.5f, h = 0.5f;
    // Slope normal: for apex at +h and rim radius r, normal tilts up by atan(r / 2h).
    const float ny = r / (2.0f * h);

    // Side: one apex vertex per sector for correct normals.
    for (uint32_t s = 0; s <= sectors; ++s) {
        float theta = 2.0f * pi * ((float)s + 0.5f) / (float)sectors;
        vec3 n = glm::normalize(vec3(std::cos(theta), ny, std::sin(theta)));
        vertices.push_back({{0, h, 0}, n, {(float)s / sectors, 1}});
    }
    uint32_t rimStart = (uint32_t)vertices.size();
    for (uint32_t s = 0; s <= sectors; ++s) {
        float theta = 2.0f * pi * (float)s / (float)sectors;
        vec3 n = glm::normalize(vec3(std::cos(theta), ny, std::sin(theta)));
        vertices.push_back({{std::cos(theta) * r, -h, std::sin(theta) * r}, n, {(float)s / sectors, 0}});
    }
    for (uint32_t s = 0; s < sectors; ++s)
        indices.insert(indices.end(), {s, rimStart + s + 1, rimStart + s});

    // Base cap.
    uint32_t center = (uint32_t)vertices.size();
    vertices.push_back({{0, -h, 0}, {0, -1, 0}, {0.5f, 0.5f}});
    for (uint32_t s = 0; s <= sectors; ++s) {
        float theta = 2.0f * pi * (float)s / (float)sectors;
        vertices.push_back({{std::cos(theta) * r, -h, std::sin(theta) * r}, {0, -1, 0},
                            {0.5f + 0.5f * std::cos(theta), 0.5f + 0.5f * std::sin(theta)}});
    }
    for (uint32_t s = 0; s < sectors; ++s)
        indices.insert(indices.end(), {center, center + 1 + s, center + 2 + s});

    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

std::shared_ptr<Mesh> MeshFactory::Torus(float minorRadius, uint32_t majorSectors, uint32_t minorSectors)
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    const float pi = glm::pi<float>();
    const float R = 0.5f - minorRadius; // overall diameter stays ~1

    for (uint32_t i = 0; i <= majorSectors; ++i) {
        float u = 2.0f * pi * (float)i / (float)majorSectors;
        vec3 ringCenter{std::cos(u) * R, 0.0f, std::sin(u) * R};
        for (uint32_t j = 0; j <= minorSectors; ++j) {
            float v = 2.0f * pi * (float)j / (float)minorSectors;
            vec3 n = glm::normalize(vec3(std::cos(u) * std::cos(v), std::sin(v), std::sin(u) * std::cos(v)));
            vertices.push_back({ringCenter + n * minorRadius, n,
                                {(float)i / majorSectors, (float)j / minorSectors}});
        }
    }

    for (uint32_t i = 0; i < majorSectors; ++i) {
        for (uint32_t j = 0; j < minorSectors; ++j) {
            uint32_t i0 = i * (minorSectors + 1) + j;
            uint32_t i1 = i0 + minorSectors + 1;
            indices.insert(indices.end(), {i0, i0 + 1, i1, i1, i0 + 1, i1 + 1});
        }
    }

    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

std::shared_ptr<Mesh> MeshFactory::Plane(float size, uint32_t subdivisions)
{
    uint32_t n = std::max(subdivisions, 1u);
    float h = size * 0.5f;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    vertices.reserve((n + 1) * (n + 1));

    for (uint32_t z = 0; z <= n; ++z) {
        for (uint32_t x = 0; x <= n; ++x) {
            float u = (float)x / n, v = (float)z / n;
            vertices.push_back({{-h + u * size, 0.0f, -h + v * size}, {0, 1, 0}, {u, v}});
        }
    }
    for (uint32_t z = 0; z < n; ++z) {
        for (uint32_t x = 0; x < n; ++x) {
            uint32_t i0 = z * (n + 1) + x;
            uint32_t i1 = i0 + n + 1;
            indices.insert(indices.end(), {i0, i1, i0 + 1, i0 + 1, i1, i1 + 1});
        }
    }
    return std::make_shared<Mesh>(std::move(vertices), std::move(indices));
}

// ---------------------------------------------------------------------------
// Text -> extruded 3D mesh
// ---------------------------------------------------------------------------

namespace {

// One closed glyph outline in font units, flattened to line segments.
struct Contour {
    std::vector<vec2> points;
    float signedArea = 0.0f; // shoelace; sign gives winding
    int depth = 0;           // how many other contours enclose it (even = outer, odd = hole)
};

float Shoelace(const std::vector<vec2>& pts)
{
    float a = 0.0f;
    for (size_t i = 0; i < pts.size(); ++i) {
        const vec2& p = pts[i];
        const vec2& q = pts[(i + 1) % pts.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5f * a;
}

bool PointInPolygon(const vec2& p, const std::vector<vec2>& poly)
{
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        if ((poly[i].y > p.y) != (poly[j].y > p.y) &&
            p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
            inside = true;
    }
    return inside;
}

} // namespace

} // namespace forge

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
#include <mapbox/earcut.hpp>

#include <array>
#include <fstream>

namespace forge {

std::shared_ptr<Mesh> MeshFactory::Text(const std::string& text, const std::string& fontPath, float depth)
{
    std::ifstream file(fontPath, std::ios::binary | std::ios::ate);
    if (!file)
        return nullptr;
    std::vector<unsigned char> fontData((size_t)file.tellg());
    file.seekg(0);
    file.read((char*)fontData.data(), (std::streamsize)fontData.size());

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, fontData.data(), stbtt_GetFontOffsetForIndex(fontData.data(), 0)))
        return nullptr;

    std::vector<Vertex> outVerts;
    std::vector<uint32_t> outIdx;

    int ascent = 0, descentI = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&font, &ascent, &descentI, &lineGap);
    const float fontHeight = (float)ascent; // cap height-ish; normalized at the end
    float penX = 0.0f;
    constexpr int kBezierSegments = 10;

    int prevCp = 0;
    for (unsigned char ch : text) {
        int cp = (int)ch; // ASCII/Latin-1 only — fine for a beginner nameplate tool
        if (prevCp)
            penX += (float)stbtt_GetCodepointKernAdvance(&font, prevCp, cp);
        prevCp = cp;

        int advance = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&font, cp, &advance, &lsb);

        stbtt_vertex* shape = nullptr;
        int numShape = stbtt_GetCodepointShape(&font, cp, &shape);
        if (numShape <= 0 || !shape) {
            penX += (float)advance;
            continue;
        }

        // --- flatten outlines to contours -----------------------------------
        std::vector<Contour> contours;
        Contour cur;
        vec2 pos{0.0f};
        bool sawCubic = false;
        auto closeContour = [&]() {
            if (cur.points.size() >= 3) {
                if (glm::distance(cur.points.front(), cur.points.back()) < 1e-3f)
                    cur.points.pop_back(); // drop duplicated closing point
                if (cur.points.size() >= 3) {
                    cur.signedArea = Shoelace(cur.points);
                    contours.push_back(std::move(cur));
                }
            }
            cur = {};
        };
        for (int s = 0; s < numShape; ++s) {
            const stbtt_vertex& v = shape[s];
            vec2 p{(float)v.x + penX, (float)v.y};
            switch (v.type) {
            case STBTT_vmove:
                closeContour();
                cur.points.push_back(p);
                pos = p;
                break;
            case STBTT_vline:
                cur.points.push_back(p);
                pos = p;
                break;
            case STBTT_vcurve: {
                vec2 c{(float)v.cx + penX, (float)v.cy};
                for (int k = 1; k <= kBezierSegments; ++k) {
                    float t = (float)k / kBezierSegments;
                    float u = 1.0f - t;
                    cur.points.push_back(pos * (u * u) + c * (2.0f * u * t) + p * (t * t));
                }
                pos = p;
                break;
            }
            default: // STBTT_vcubic: CFF outlines unsupported
                sawCubic = true;
                break;
            }
        }
        closeContour();
        stbtt_FreeShape(&font, shape);
        penX += (float)advance;
        if (sawCubic)
            return nullptr; // reject CFF fonts wholesale rather than emit garbage

        // --- classify outer vs hole by containment depth (winding untrusted) --
        for (size_t i = 0; i < contours.size(); ++i)
            for (size_t j = 0; j < contours.size(); ++j)
                if (i != j && PointInPolygon(contours[i].points[0], contours[j].points))
                    ++contours[i].depth;

        // Force orientation: outers CCW, holes CW (walls below rely on it).
        for (Contour& c : contours) {
            bool isHole = (c.depth % 2) == 1;
            bool ccw = c.signedArea > 0.0f;
            if (isHole == ccw)
                std::reverse(c.points.begin(), c.points.end());
        }

        // --- per outer: earcut caps + wall strips -----------------------------
        const float zF = depth * 0.5f * fontHeight, zB = -zF; // scaled with font units, normalized later
        for (size_t i = 0; i < contours.size(); ++i) {
            if (contours[i].depth % 2)
                continue; // holes are emitted with their outer

            using EPoint = std::array<float, 2>;
            std::vector<std::vector<EPoint>> polygon;
            std::vector<const Contour*> rings{&contours[i]};
            // A hole belongs to this outer iff it sits inside it one level deeper.
            for (size_t j = 0; j < contours.size(); ++j)
                if (j != i && contours[j].depth == contours[i].depth + 1 &&
                    PointInPolygon(contours[j].points[0], contours[i].points))
                    rings.push_back(&contours[j]);
            for (const Contour* r : rings) {
                std::vector<EPoint> ring;
                ring.reserve(r->points.size());
                for (const vec2& p : r->points)
                    ring.push_back({p.x, p.y});
                polygon.push_back(std::move(ring));
            }

            std::vector<uint32_t> capIdx = mapbox::earcut<uint32_t>(polygon);
            if (capIdx.empty())
                continue;

            // Front cap verts (rings flattened, matching earcut's index space).
            uint32_t frontBase = (uint32_t)outVerts.size();
            for (const Contour* r : rings)
                for (const vec2& p : r->points)
                    outVerts.push_back({{p.x, p.y, zF}, {0, 0, 1}, {0, 0}});
            uint32_t backBase = (uint32_t)outVerts.size();
            for (const Contour* r : rings)
                for (const vec2& p : r->points)
                    outVerts.push_back({{p.x, p.y, zB}, {0, 0, -1}, {0, 0}});

            // earcut output follows the outer ring's CCW orientation -> +z facing.
            for (size_t k = 0; k + 2 < capIdx.size(); k += 3) {
                outIdx.insert(outIdx.end(),
                              {frontBase + capIdx[k], frontBase + capIdx[k + 1], frontBase + capIdx[k + 2]});
                outIdx.insert(outIdx.end(),
                              {backBase + capIdx[k + 2], backBase + capIdx[k + 1], backBase + capIdx[k]});
            }

            // Walls: fresh vertices per edge for hard side edges.
            for (const Contour* r : rings) {
                const auto& pts = r->points;
                for (size_t k = 0; k < pts.size(); ++k) {
                    const vec2& a = pts[k];
                    const vec2& b = pts[(k + 1) % pts.size()];
                    vec2 dir = b - a;
                    float len = glm::length(dir);
                    if (len < 1e-6f)
                        continue;
                    vec2 nrm2{dir.y / len, -dir.x / len}; // right of travel = outward for CCW outer
                    vec3 n{nrm2.x, nrm2.y, 0.0f};
                    uint32_t base = (uint32_t)outVerts.size();
                    outVerts.push_back({{a.x, a.y, zF}, n, {0, 0}});
                    outVerts.push_back({{b.x, b.y, zF}, n, {0, 0}});
                    outVerts.push_back({{a.x, a.y, zB}, n, {0, 0}});
                    outVerts.push_back({{b.x, b.y, zB}, n, {0, 0}});
                    // (A, A', B') + (A, B', B): outward for the orientation forced above
                    outIdx.insert(outIdx.end(), {base, base + 2, base + 3, base, base + 3, base + 1});
                }
            }
        }
    }

    if (outIdx.empty())
        return nullptr;

    // Normalize: glyph height ~1 unit, centered at origin.
    vec3 mn(FLT_MAX), mx(-FLT_MAX);
    for (const Vertex& v : outVerts) {
        mn = glm::min(mn, v.position);
        mx = glm::max(mx, v.position);
    }
    float s = 1.0f / fontHeight;
    vec3 center = (mn + mx) * 0.5f;
    for (Vertex& v : outVerts)
        v.position = (v.position - center) * s;

    return std::make_shared<Mesh>(std::move(outVerts), std::move(outIdx));
}

} // namespace forge
