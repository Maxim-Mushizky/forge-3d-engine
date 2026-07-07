#include "MeshBuild.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace forge {

namespace {

constexpr float kWeldEps = 1e-6f; // consecutive input points closer than this collapse
constexpr float kAxisEps = 1e-6f; // lathe radius at/below this sits on the axis (pole)

// Point lists come straight from json/Lua, so a buggy caller loop can hand us
// hundreds of thousands of points; the build runs serially on the GL main
// thread, so oversized input must fail fast instead of allocating gigabytes.
constexpr size_t kMaxProfilePoints = 4096;
constexpr size_t kMaxPathPoints = 16384;
constexpr size_t kMaxVertices = 2'000'000; // projected ring grid, pre-caps

bool Finite(const vec2& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y);
}

bool Finite(const vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Zero-length segments would produce degenerate quads and NaN tangents, so
// consecutive duplicates are dropped up front instead of special-cased later.
template <typename V>
std::vector<V> DedupeConsecutive(const std::vector<V>& pts)
{
    std::vector<V> result;
    result.reserve(pts.size());
    for (const V& p : pts)
        if (result.empty() || glm::length(p - result.back()) > kWeldEps)
            result.push_back(p);
    return result;
}

// One shared angle table per build: the seam column (s == sectors) reuses the
// s == 0 entries, so the duplicated UV row lands on bit-identical positions
// and position-welding consumers see a closed ring, not a phantom boundary.
void FillAngleTable(uint32_t sectors, std::vector<float>& cs, std::vector<float>& sn)
{
    cs.resize(sectors + 1);
    sn.resize(sectors + 1);
    const float twoPi = 2.0f * glm::pi<float>();
    for (uint32_t s = 0; s < sectors; ++s) {
        const float theta = twoPi * (float)s / (float)sectors;
        cs[s] = std::cos(theta);
        sn[s] = std::sin(theta);
    }
    cs[sectors] = cs[0];
    sn[sectors] = sn[0];
}

} // namespace

bool BuildLathe(const std::vector<vec2>& profile, uint32_t sectors, bool closed, MeshData& out)
{
    out = {};
    for (const vec2& p : profile)
        if (!Finite(p) || p.x < 0.0f)
            return false;

    std::vector<vec2> pts = DedupeConsecutive(profile);
    if (pts.size() < 2 || pts.size() > kMaxProfilePoints)
        return false;
    float maxRadius = 0.0f;
    for (const vec2& p : pts)
        maxRadius = std::max(maxRadius, p.x);
    if (maxRadius <= kAxisEps)
        return false; // every point on the axis: revolving yields no surface
    sectors = std::clamp(sectors, 3u, 1024u);
    if (pts.size() * (size_t)(sectors + 1) > kMaxVertices)
        return false;

    // A profile that returns to its start is a loop (revolved ring): the wall
    // closes itself, so caps would stack coincident opposite-facing disks on
    // the welded seam ring and break the manifold. Snap the closure bit-exact
    // so the position weld actually sees one ring.
    const bool loop = pts.size() >= 3 && glm::length(pts.front() - pts.back()) <= kWeldEps;
    if (loop)
        pts.back() = pts.front();

    std::vector<float> cs, sn;
    FillAngleTable(sectors, cs, sn);

    // v runs 0..1 along the profile by arc length, u around the revolution.
    std::vector<float> vCoord(pts.size(), 0.0f);
    for (size_t i = 1; i < pts.size(); ++i)
        vCoord[i] = vCoord[i - 1] + glm::length(pts[i] - pts[i - 1]);
    const float totalLen = vCoord.back();
    for (float& v : vCoord)
        v /= totalLen; // > 0: dedupe left at least two distinct points

    // Per-point profile normals from central differences: rotating the profile
    // tangent -90 degrees points +r on an ascending outer wall and toward the
    // axis on a descending inner wall, so cup interiors light correctly. Loops
    // wrap around the closure so both seam rings shade identically.
    const size_t nProf = pts.size();
    std::vector<vec2> pn(nProf);
    for (size_t i = 0; i < nProf; ++i) {
        const vec2 prev = loop ? pts[i > 0 ? i - 1 : nProf - 2] : pts[i > 0 ? i - 1 : i];
        const vec2 next = loop ? pts[i + 1 < nProf ? i + 1 : 1] : pts[i + 1 < nProf ? i + 1 : i];
        vec2 d = next - prev;
        // A switchback (prev == next around a turnaround point) collapses the
        // central difference to zero; normalize would emit NaN normals. Fall
        // back to a one-sided difference — dedupe guarantees it is nonzero.
        if (glm::length(d) <= kWeldEps)
            d = i + 1 < nProf ? pts[i + 1] - pts[i] : pts[i] - pts[i - 1];
        const vec2 t = glm::normalize(d);
        pn[i] = vec2(t.y, -t.x);
    }

    const uint32_t ringStride = sectors + 1;
    out.vertices.reserve(pts.size() * ringStride);
    for (size_t i = 0; i < pts.size(); ++i) {
        const float r = pts[i].x, y = pts[i].y;
        for (uint32_t s = 0; s <= sectors; ++s) {
            // r = 0 times a negative cos/sin gives -0.0f, whose bits differ
            // from +0.0f — bit-exact welds would see a split pole (#117).
            const vec3 pos = r <= kAxisEps ? vec3(0.0f, y, 0.0f)
                                           : vec3(r * cs[s], y, r * sn[s]);
            const vec3 n = glm::normalize(vec3(pn[i].x * cs[s], pn[i].y, pn[i].x * sn[s]));
            out.vertices.push_back({pos, n, {(float)s / sectors, vCoord[i]}});
        }
    }

    // Counter-clockwise seen from outside (#116). Pole rings collapse one quad
    // edge, so only the non-degenerate half is emitted there (sphere-style).
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const bool bottomPole = pts[i].x <= kAxisEps;
        const bool topPole = pts[i + 1].x <= kAxisEps;
        if (bottomPole && topPole)
            continue; // segment runs along the axis: nothing to revolve
        for (uint32_t s = 0; s < sectors; ++s) {
            const uint32_t a = (uint32_t)i * ringStride + s; // ring i, this sector
            const uint32_t b = a + 1;                        // ring i, next sector
            const uint32_t c = a + ringStride;               // ring i+1, this sector
            const uint32_t d = c + 1;                        // ring i+1, next sector
            if (!topPole)
                out.indices.insert(out.indices.end(), {c, d, a});
            if (!bottomPole)
                out.indices.insert(out.indices.end(), {a, d, b});
        }
    }

    if (closed && !loop) {
        // Fan caps over open ends; r = 0 ends are already sealed by the pole
        // collapse and loops close themselves. Ring positions repeat the wall
        // expressions bit-exactly.
        for (int end = 0; end < 2; ++end) {
            const vec2 p = end == 0 ? pts.front() : pts.back();
            if (p.x <= kAxisEps)
                continue;
            const vec3 n{0.0f, end == 0 ? -1.0f : 1.0f, 0.0f};
            const uint32_t center = (uint32_t)out.vertices.size();
            out.vertices.push_back({{0.0f, p.y, 0.0f}, n, {0.5f, 0.5f}});
            for (uint32_t s = 0; s <= sectors; ++s)
                out.vertices.push_back({{p.x * cs[s], p.y, p.x * sn[s]}, n,
                                        {0.5f + 0.5f * cs[s], 0.5f + 0.5f * sn[s]}});
            for (uint32_t s = 0; s < sectors; ++s) {
                if (end == 0)
                    out.indices.insert(out.indices.end(), {center, center + 1 + s, center + 2 + s});
                else
                    out.indices.insert(out.indices.end(), {center, center + 2 + s, center + 1 + s});
            }
        }
    }

    return true;
}

// Ear-clipping triangulation of a simple CCW polygon; returns index triples
// into sec. Sweep caps used to fan around the section centroid, which only
// covers sections star-shaped about it — outlines traced from reference
// images (#135: an axe head with concave notches between its blades) are not,
// and the fan overfilled every concavity. Ear containment tests check REFLEX
// vertices only — the standard optimization that makes the whole run O(n^2)
// worst case instead of O(n^3). All orientation/containment math runs in
// double: pixel-derived sections carry long near-collinear runs that float
// cross products misclassify.
static std::vector<uint32_t> TriangulateSection(const std::vector<vec2>& sec)
{
    const uint32_t n = (uint32_t)sec.size();
    std::vector<uint32_t> tris;
    if (n < 3)
        return tris;
    tris.reserve(3 * (size_t)(n - 2));

    // Doubly-linked ring over the live vertices.
    std::vector<uint32_t> prev(n), next(n);
    for (uint32_t i = 0; i < n; ++i) {
        prev[i] = (i + n - 1) % n;
        next[i] = (i + 1) % n;
    }
    // Twice the signed area of corner (prev, i, next); > 0 = convex on a CCW
    // ring. Collinear corners (== 0) count as reflex: they can still BLOCK an
    // ear (conservative) but are never clipped as one — the degenerate-emit
    // skip below consumes them.
    auto cross = [&](uint32_t i) {
        const vec2& a = sec[prev[i]];
        const vec2& b = sec[i];
        const vec2& c = sec[next[i]];
        return ((double)b.x - a.x) * ((double)c.y - b.y) -
               ((double)b.y - a.y) * ((double)c.x - b.x);
    };
    std::vector<char> reflex(n);
    for (uint32_t i = 0; i < n; ++i)
        reflex[i] = cross(i) <= 0.0;

    // A convex corner is an ear when no reflex vertex lies inside its triangle
    // (if ANY vertex lies inside an ear candidate, some reflex one does, so
    // convex vertices need no test). A point exactly ON a triangle edge counts
    // as blocking — clipping through a touching vertex would leave a zero-width
    // sliver or a self-overlap. The ring walk from next[next[i]] to prev[i]
    // structurally skips the triangle's own three vertices.
    auto isEar = [&](uint32_t i) {
        if (reflex[i])
            return false;
        const vec2& a = sec[prev[i]];
        const vec2& b = sec[i];
        const vec2& c = sec[next[i]];
        for (uint32_t v = next[next[i]]; v != prev[i]; v = next[v]) {
            if (!reflex[v])
                continue;
            const vec2& p = sec[v];
            const double d0 = ((double)b.x - a.x) * ((double)p.y - a.y) -
                              ((double)b.y - a.y) * ((double)p.x - a.x);
            const double d1 = ((double)c.x - b.x) * ((double)p.y - b.y) -
                              ((double)c.y - b.y) * ((double)p.x - b.x);
            const double d2 = ((double)a.x - c.x) * ((double)p.y - c.y) -
                              ((double)a.y - c.y) * ((double)p.x - c.x);
            if (d0 >= 0.0 && d1 >= 0.0 && d2 >= 0.0)
                return false;
        }
        return true;
    };

    uint32_t live = n;
    uint32_t cur = 0;
    uint32_t sinceClip = 0;
    auto clip = [&](uint32_t i) {
        // A ~zero-area corner (collinear or duplicate points) still unlinks so
        // the run is consumed — it just contributes no triangle.
        if (std::fabs(cross(i)) > 1e-12)
            tris.insert(tris.end(), {prev[i], i, next[i]});
        const uint32_t p = prev[i], nx = next[i];
        next[p] = nx;
        prev[nx] = p;
        --live;
        // Unlinking changes exactly two corner angles; everything else keeps
        // its convex/reflex status.
        reflex[p] = cross(p) <= 0.0;
        reflex[nx] = cross(nx) <= 0.0;
        cur = p; // the predecessor may have just become an ear
        sinceClip = 0;
    };
    while (live > 3) {
        if (isEar(cur)) {
            clip(cur);
            continue;
        }
        cur = next[cur];
        if (++sinceClip < live)
            continue;
        // A full cycle found no clippable ear: numerically degenerate input
        // (collinear runs, duplicate points). Force-clip the flattest convex
        // corner — smallest |cross| — or the current vertex if none is convex,
        // so the loop always terminates: builds run serially on the GL main
        // thread and must never hang, the walls are already built, and a
        // slightly wrong cap on broken input beats no mesh.
        uint32_t best = cur;
        double bestAbs = std::numeric_limits<double>::max();
        uint32_t v = cur;
        do {
            if (!reflex[v]) {
                const double a = std::fabs(cross(v));
                if (a < bestAbs) {
                    bestAbs = a;
                    best = v;
                }
            }
            v = next[v];
        } while (v != cur);
        clip(best);
    }
    // The last three live vertices are the final triangle.
    if (std::fabs(cross(cur)) > 1e-12)
        tris.insert(tris.end(), {prev[cur], cur, next[cur]});
    return tris;
}

bool BuildSweep(const std::vector<vec2>& profile, const std::vector<vec3>& path, MeshData& out)
{
    out = {};
    for (const vec2& p : profile)
        if (!Finite(p))
            return false;
    for (const vec3& p : path)
        if (!Finite(p))
            return false;

    std::vector<vec2> sec = DedupeConsecutive(profile);
    if (sec.size() >= 2 && glm::length(sec.front() - sec.back()) <= kWeldEps)
        sec.pop_back(); // tolerate an explicitly closed input polygon
    if (sec.size() < 3 || sec.size() > kMaxProfilePoints)
        return false;
    double area2 = 0.0; // shoelace, twice the signed area; double so huge-but-
                        // finite floats can't overflow to inf/NaN and sneak a
                        // backwards section past the checks below
    for (size_t j = 0; j < sec.size(); ++j) {
        const vec2& p0 = sec[j];
        const vec2& p1 = sec[(j + 1) % sec.size()];
        area2 += (double)p0.x * p1.y - (double)p1.x * p0.y;
    }
    if (!std::isfinite(area2) || std::fabs(area2) < 1e-8)
        return false; // collinear section: no enclosed area to extrude
    if (area2 < 0.0)
        std::reverse(sec.begin(), sec.end()); // normalize to CCW so winding below holds

    const std::vector<vec3> pathPts = DedupeConsecutive(path);
    if (pathPts.size() < 2 || pathPts.size() > kMaxPathPoints)
        return false;
    const size_t nPath = pathPts.size();
    if (nPath * (sec.size() + 1) > kMaxVertices)
        return false;

    std::vector<vec3> tan(nPath);
    for (size_t i = 0; i < nPath; ++i) {
        const vec3 prev = pathPts[i > 0 ? i - 1 : i];
        const vec3 next = pathPts[i + 1 < nPath ? i + 1 : i];
        vec3 d = next - prev;
        // Switchback (out-and-back path): prev == next collapses the central
        // difference and normalize would poison every frame — and therefore
        // every ring position — with NaN. One-sided fallback is nonzero after
        // dedupe.
        if (glm::length(d) <= kWeldEps)
            d = i + 1 < nPath ? pathPts[i + 1] - pathPts[i] : pathPts[i] - pathPts[i - 1];
        tan[i] = glm::normalize(d);
    }

    // Parallel transport: rotate the previous frame by exactly the rotation
    // between consecutive tangents (Rodrigues), so the section never spins
    // around the path — a Frenet frame would flip at inflection points.
    // Seed convention (#138): the section reads as drawn when looking back
    // along the start tangent with world +Y as screen-up — section x is
    // screen-right (nor), section y is screen-up (bin). A +Z path therefore
    // maps section (x, y) straight onto world (x, y). Near-vertical paths
    // fall back to a -sign(tangent.y)*Z up hint: section (x, y) maps to
    // world (x, -z) going up and (x, z) going down, which is bit-exactly
    // the frame every pre-#138 vertical sweep already had in BOTH directions
    // (an unsigned hint would rotate legacy downward sweeps 180 degrees).
    std::vector<vec3> nor(nPath), bin(nPath);
    const vec3 up = std::fabs(tan[0].y) < 0.9f ? vec3(0, 1, 0)
                                               : vec3(0, 0, tan[0].y > 0.0f ? -1.0f : 1.0f);
    bin[0] = glm::normalize(up - tan[0] * glm::dot(up, tan[0]));
    nor[0] = glm::cross(bin[0], tan[0]); // keeps b = t x n, so winding below holds
    for (size_t i = 1; i < nPath; ++i) {
        const vec3 axis = glm::cross(tan[i - 1], tan[i]);
        const float sinA = glm::length(axis);
        if (sinA < 1e-8f) {
            nor[i] = nor[i - 1]; // straight (or reversed) segment: keep the frame
        } else {
            const vec3 k = axis / sinA;
            const float cosA = glm::clamp(glm::dot(tan[i - 1], tan[i]), -1.0f, 1.0f);
            const vec3 v = nor[i - 1];
            nor[i] = glm::normalize(v * cosA + glm::cross(k, v) * sinA +
                                    k * glm::dot(k, v) * (1.0f - cosA));
        }
        bin[i] = glm::cross(tan[i], nor[i]);
    }

    // Smooth per-vertex section normals: outward edge normal of a CCW polygon
    // is the edge direction rotated -90 degrees.
    const size_t k = sec.size();
    std::vector<vec2> edgeN(k), vertN(k);
    for (size_t j = 0; j < k; ++j) {
        const vec2 e = sec[(j + 1) % k] - sec[j];
        edgeN[j] = glm::normalize(vec2(e.y, -e.x));
    }
    for (size_t j = 0; j < k; ++j) {
        const vec2 sum = edgeN[(j + k - 1) % k] + edgeN[j];
        vertN[j] = glm::length(sum) > 1e-6f ? glm::normalize(sum) : edgeN[j];
    }

    std::vector<float> vCoord(nPath, 0.0f);
    for (size_t i = 1; i < nPath; ++i)
        vCoord[i] = vCoord[i - 1] + glm::length(pathPts[i] - pathPts[i - 1]);
    for (float& v : vCoord)
        v /= vCoord.back();

    // Rings: k section points plus a seam duplicate for the UV wrap; the seam
    // vertex reuses index 0's expressions, so its position is bit-identical.
    const uint32_t ringStride = (uint32_t)k + 1;
    out.vertices.reserve(nPath * ringStride);
    for (size_t i = 0; i < nPath; ++i) {
        for (size_t j = 0; j <= k; ++j) {
            const size_t jj = j % k;
            const vec3 pos = pathPts[i] + nor[i] * sec[jj].x + bin[i] * sec[jj].y;
            const vec3 n = glm::normalize(nor[i] * vertN[jj].x + bin[i] * vertN[jj].y);
            out.vertices.push_back({pos, n, {(float)j / (float)k, vCoord[i]}});
        }
    }

    // Counter-clockwise from outside for a CCW section and b = t x n frames.
    for (size_t i = 0; i + 1 < nPath; ++i) {
        for (uint32_t j = 0; j < (uint32_t)k; ++j) {
            const uint32_t a = (uint32_t)i * ringStride + j;
            const uint32_t b = a + 1;
            const uint32_t c = a + ringStride;
            const uint32_t d = c + 1;
            out.indices.insert(out.indices.end(), {a, b, c, b, d, c});
        }
    }

    // Ear-clipped caps (an outline traced from a reference image, #135, is
    // routinely concave — the old centroid fan overfilled every notch). Ring
    // positions repeat the wall expressions bit-exactly so cap rings weld
    // onto wall rings. k vertices per cap, not k+1: the seam duplicate only
    // existed so fan triangles j..j+1 could wrap past index 0, and cap UVs
    // are planar — there is no UV seam to split; ear triples index arbitrary
    // vertex pairs directly. The fan's centroid vertex is gone entirely.
    vec2 lo = sec[0], hi = sec[0];
    for (const vec2& p : sec) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const vec2 extent = glm::max(hi - lo, vec2(1e-6f));
    const std::vector<uint32_t> capTris = TriangulateSection(sec);
    for (int end = 0; end < 2; ++end) {
        const size_t i = end == 0 ? 0 : nPath - 1;
        const vec3 n = end == 0 ? -tan[i] : tan[i];
        const uint32_t base = (uint32_t)out.vertices.size();
        for (size_t j = 0; j < k; ++j)
            out.vertices.push_back({pathPts[i] + nor[i] * sec[j].x + bin[i] * sec[j].y, n,
                                    (sec[j] - lo) / extent});
        // Ear triples are CCW in section space, whose +z is tan (nor x bin =
        // tan): as-is they face the +tan end; the -tan start cap flips them —
        // the same winding flip the fan caps did.
        for (size_t t = 0; t + 2 < capTris.size(); t += 3) {
            if (end == 0)
                out.indices.insert(out.indices.end(), {base + capTris[t], base + capTris[t + 2],
                                                       base + capTris[t + 1]});
            else
                out.indices.insert(out.indices.end(), {base + capTris[t], base + capTris[t + 1],
                                                       base + capTris[t + 2]});
        }
    }

    return true;
}

} // namespace forge
