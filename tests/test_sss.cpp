#include "test_framework.h"

#include "forge/raytrace/Sss.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace forge::test {

namespace {

// Van de Hulst's approximation for the multi-scattered reflectance of a
// semi-infinite medium — the function SssSingleScatterAlbedo inverts.
float VanDeHulstReflectance(float alpha)
{
    const float s = std::sqrt(1.0f - alpha);
    return (1.0f - s) * (1.0f - 0.139f * s) / (1.0f + 1.17f * s);
}

// Deterministic PCG32 (same construction as the shader's) so the Monte Carlo
// check below never flakes.
uint32_t Pcg(uint32_t& state)
{
    state = state * 747796405u + 2891336453u;
    uint32_t w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (w >> 22u) ^ w;
}

float Rand01(uint32_t& state)
{
    return (float)Pcg(state) / 4294967296.0f;
}

// Analog random walk in a semi-infinite half-space medium (z < 0), isotropic
// phase, unit extinction, per-scatter survival weight = alpha. The mean exit
// weight is the medium's multi-scattered reflectance — the quantity the van
// de Hulst inversion is built to hit. Validates the shipped mapping
// end-to-end, not just its algebra.
float WalkReflectance(float alpha, int walkers, uint32_t seed)
{
    double sum = 0.0;
    for (int i = 0; i < walkers; ++i) {
        // Cosine-weighted entry through the surface (diffuse transmission),
        // matching the path tracer's entry bounce.
        float r1 = Rand01(seed), r2 = Rand01(seed);
        float phi = 6.2831853f * r1;
        float sr = std::sqrt(r2);
        vec3 dir{sr * std::cos(phi), sr * std::sin(phi), -std::sqrt(1.0f - r2)};
        vec3 pos{0.0f, 0.0f, 0.0f};
        double weight = 1.0;
        for (int step = 0; step < 10000; ++step) {
            float t = -std::log(std::max(1.0f - Rand01(seed), 1e-7f));
            if (dir.z > 0.0f && pos.z + dir.z * t >= 0.0f) {
                sum += weight; // crossed back out before the next scatter
                break;
            }
            pos += dir * t;
            weight *= alpha;
            if (weight < 1e-6)
                break; // absorbed
            float z = 1.0f - 2.0f * Rand01(seed);
            float p = 6.2831853f * Rand01(seed);
            float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            dir = {r * std::cos(p), r * std::sin(p), z};
        }
    }
    return (float)(sum / walkers);
}

// Chromatic variant mirroring pathtrace.comp's estimator verbatim: per-step
// channel selection proportional to throughput*alpha, balance-heuristic
// mixture pdf, transmittance wall weights, and the same 1e3 per-step cap.
// Exercises the dead-channel case the analog walk above can't reach (a
// never-sampled channel crossing a wall gets a numerator with no alpha
// factor — unbounded without the cap).
struct RgbWalkResult {
    vec3 mean{0.0f};
    float maxWeight = 0.0f; // largest single-walk exit weight, any channel
};

RgbWalkResult WalkReflectanceRGB(const vec3& alpha, const vec3& sigmaT, int walkers,
                                 uint32_t seed)
{
    RgbWalkResult res;
    double sum[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < walkers; ++i) {
        float r1 = Rand01(seed), r2 = Rand01(seed);
        float phi = 6.2831853f * r1;
        float sr = std::sqrt(r2);
        vec3 dir{sr * std::cos(phi), sr * std::sin(phi), -std::sqrt(1.0f - r2)};
        vec3 pos{0.0f, 0.0f, 0.0f};
        vec3 tp{1.0f, 1.0f, 1.0f};
        for (int step = 0; step < 64; ++step) {
            vec3 cw = glm::abs(tp * alpha);
            float cwSum = cw.x + cw.y + cw.z;
            vec3 chPdf = cwSum > 0.0f ? cw / cwSum : vec3(1.0f / 3.0f);
            float cr = Rand01(seed);
            float sigma = cr < chPdf.x ? sigmaT.x : (cr < chPdf.x + chPdf.y ? sigmaT.y : sigmaT.z);
            float t = -std::log(std::max(1.0f - Rand01(seed), 1e-7f)) / sigma;

            float wallT = dir.z > 0.0f ? -pos.z / dir.z : 1e30f;
            bool wall = wallT < t;
            float wt = wall ? wallT : t;
            vec3 tr = glm::exp(-sigmaT * wt);
            vec3 pdf = wall ? tr : sigmaT * tr;
            tp *= glm::min((wall ? tr : alpha * sigmaT * tr) /
                               std::max(glm::dot(chPdf, pdf), 1e-12f),
                           vec3(1e3f));
            pos += dir * wt;
            if (wall) {
                sum[0] += tp.x;
                sum[1] += tp.y;
                sum[2] += tp.z;
                res.maxWeight = std::max({res.maxWeight, tp.x, tp.y, tp.z});
                break;
            }
            if (std::max({tp.x, tp.y, tp.z}) < 1e-6f)
                break;
            float z = 1.0f - 2.0f * Rand01(seed);
            float p = 6.2831853f * Rand01(seed);
            float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
            dir = {r * std::cos(p), r * std::sin(p), z};
        }
    }
    res.mean = {(float)(sum[0] / walkers), (float)(sum[1] / walkers), (float)(sum[2] / walkers)};
    return res;
}

} // namespace

void RunSssTests()
{
    // --- inversion round trip: R(alpha(A)) == A ------------------------------
    {
        for (float a = 0.05f; a < 0.96f; a += 0.05f) {
            vec3 alpha = SssSingleScatterAlbedo(vec3(a));
            CHECK(ApproxEq(VanDeHulstReflectance(alpha.x), a, 1e-3f));
        }
    }

    // --- endpoints and clamping ----------------------------------------------
    {
        vec3 black = SssSingleScatterAlbedo(vec3(0.0f));
        CHECK(ApproxEq(black.x, 0.0f, 1e-5f)); // no scattering: all absorbed
        vec3 white = SssSingleScatterAlbedo(vec3(1.0f));
        CHECK(white.x <= 0.999999f); // clamp keeps pure white slightly absorbing
        CHECK(white.x > 0.999f);
        vec3 over = SssSingleScatterAlbedo(vec3(2.0f)); // out-of-range input clamped
        CHECK(over.x <= 0.999999f);
        vec3 under = SssSingleScatterAlbedo(vec3(-1.0f));
        CHECK(ApproxEq(under.x, 0.0f, 1e-5f));
    }

    // --- monotonic: more color demands more scattering ------------------------
    {
        float prev = -1.0f;
        for (float a = 0.0f; a <= 1.001f; a += 0.1f) {
            float alpha = SssSingleScatterAlbedo(vec3(a)).x;
            CHECK(alpha >= prev);
            prev = alpha;
        }
    }

    // --- per-channel independence ---------------------------------------------
    {
        vec3 alpha = SssSingleScatterAlbedo({0.2f, 0.5f, 0.8f});
        CHECK(ApproxEq(alpha.x, SssSingleScatterAlbedo(vec3(0.2f)).x));
        CHECK(ApproxEq(alpha.y, SssSingleScatterAlbedo(vec3(0.5f)).y));
        CHECK(ApproxEq(alpha.z, SssSingleScatterAlbedo(vec3(0.8f)).z));
        CHECK(alpha.x < alpha.y && alpha.y < alpha.z);
    }

    // --- extinction: reciprocal mean free path, floored ------------------------
    {
        vec3 sigma = SssExtinction({0.5f, 0.25f, 0.125f});
        CHECK(ApproxEq(sigma.x, 2.0f));
        CHECK(ApproxEq(sigma.y, 4.0f));
        CHECK(ApproxEq(sigma.z, 8.0f));
        vec3 zero = SssExtinction(vec3(0.0f)); // degenerate radius: finite via floor
        CHECK(ApproxEq(zero.x, 1.0f / kSssMinRadius, 1e-2f));
        vec3 neg = SssExtinction(vec3(-1.0f)); // hostile input: same floor
        CHECK(ApproxEq(neg.x, 1.0f / kSssMinRadius, 1e-2f));
    }

    // --- Monte Carlo: a walk fed the mapped alpha scatters out looking like the
    // requested color (semi-infinite slab, the configuration the fit targets).
    // Tolerance covers the fit's documented 2-5% error plus MC noise.
    {
        const int kWalkers = 20000;
        for (float a : {0.2f, 0.5f, 0.8f}) {
            float alpha = SssSingleScatterAlbedo(vec3(a)).x;
            float r = WalkReflectance(alpha, kWalkers, 0xC0FFEEu);
            CHECK(std::fabs(r - a) < 0.05f);
        }
    }

    // --- Chromatic MIS estimator (the shader's actual math) --------------------
    {
        const int kWalkers = 20000;
        // Gray medium: the 3-channel estimator must agree with the analog walk.
        {
            float a = 0.5f;
            vec3 alpha = SssSingleScatterAlbedo(vec3(a));
            RgbWalkResult rgb = WalkReflectanceRGB(alpha, vec3(1.0f), kWalkers, 0xC0FFEEu);
            CHECK(std::fabs(rgb.mean.x - a) < 0.05f);
            CHECK(std::fabs(rgb.mean.y - rgb.mean.x) < 0.02f); // channels identical by symmetry
            CHECK(std::fabs(rgb.mean.z - rgb.mean.x) < 0.02f);
        }
        // Saturated blue + red-deepest default radius: the worst dead-channel
        // configuration (near-zero red/green alpha, red flies furthest). The
        // sampled channel must still converge; the dead channels stay dark; and
        // the per-step cap keeps the largest single exit weight bounded —
        // without it this exact setup produced ~1e3 single-STEP ratios that
        // compound (review #131 finding 1).
        {
            vec3 alpha = SssSingleScatterAlbedo({0.0f, 0.0f, 1.0f});
            vec3 sigmaT = SssExtinction({0.1f, 0.05f, 0.03f});
            RgbWalkResult rgb = WalkReflectanceRGB(alpha, sigmaT, kWalkers, 0xC0FFEEu);
            CHECK(rgb.mean.z > 0.75f && rgb.mean.z <= 1.02f); // 64-step cap darkens a little
            CHECK(rgb.mean.x < 0.06f);
            CHECK(rgb.mean.y < 0.06f);
            CHECK(rgb.maxWeight < 2e3f); // bounded by the per-step cap
        }
    }

    std::printf("[ok] sss tests done\n");
}

} // namespace forge::test
