#version 460 core

in vec3 v_WorldPos;
in vec3 v_Normal;
in vec2 v_UV;
in vec4 v_LightSpacePos;

uniform vec3 u_CamPos;
uniform vec3 u_LightDir;
uniform vec3 u_LightColor;
uniform float u_LightIntensity;

struct PointLight {
    vec3 position;
    vec3 color; // premultiplied by intensity
    float range;
};
uniform int u_NumPointLights;
uniform PointLight u_PointLights[8];

uniform vec3 u_Albedo;
uniform float u_Metallic;
uniform float u_Roughness;
uniform vec3 u_Emissive; // premultiplied by strength
uniform sampler2D u_AlbedoMap;
uniform int u_HasAlbedoMap;
uniform sampler2D u_MRMap; // glTF packing: G=roughness, B=metallic
uniform int u_HasMRMap;

uniform sampler2D u_ShadowMap;
uniform int u_ShadowsEnabled;

uniform sampler2D u_EnvIrradiance;
uniform sampler2D u_EnvPrefiltered;
uniform int u_HasEnv;
uniform float u_EnvIntensity;
uniform float u_EnvRotation;

out vec4 FragColor;

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
           GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

float ShadowFactor(vec3 N, vec3 L)
{
    if (u_ShadowsEnabled == 0)
        return 1.0;
    vec3 proj = v_LightSpacePos.xyz / v_LightSpacePos.w * 0.5 + 0.5;
    if (proj.z > 1.0)
        return 1.0;
    float bias = max(0.0025 * (1.0 - dot(N, L)), 0.0005);
    vec2 texel = 1.0 / vec2(textureSize(u_ShadowMap, 0));
    float lit = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            float depth = texture(u_ShadowMap, proj.xy + vec2(x, y) * texel).r;
            lit += proj.z - bias > depth ? 0.0 : 1.0;
        }
    return lit / 9.0;
}

vec2 dirToEnvUV(vec3 d)
{
    return vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5 + u_EnvRotation / (2.0 * PI),
                acos(clamp(d.y, -1.0, 1.0)) / PI);
}

// Karis analytic environment BRDF (replaces the LUT).
vec3 EnvBRDFApprox(vec3 F0, float roughness, float NoV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}

// One light's Cook-Torrance contribution (radiance already includes intensity/attenuation).
vec3 BRDF(vec3 N, vec3 V, vec3 L, vec3 albedo, float metallic, float roughness, vec3 radiance)
{
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    if (NdotL <= 0.0)
        return vec3(0.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * NdotL + 0.0001);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / PI + specular) * radiance * NdotL;
}

void main()
{
    vec3 albedo = u_Albedo;
    if (u_HasAlbedoMap == 1)
        albedo *= texture(u_AlbedoMap, v_UV).rgb;

    float metallic = u_Metallic;
    float roughness = u_Roughness;
    if (u_HasMRMap == 1) {
        vec3 mr = texture(u_MRMap, v_UV).rgb;
        roughness *= mr.g;
        metallic *= mr.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);

    vec3 N = normalize(v_Normal);
    vec3 V = normalize(u_CamPos - v_WorldPos);

    // Sun (with shadow map).
    vec3 L = normalize(-u_LightDir);
    vec3 Lo = BRDF(N, V, L, albedo, metallic, roughness, u_LightColor * u_LightIntensity) * ShadowFactor(N, L);

    // Point lights (UE-style windowed inverse-square falloff).
    for (int i = 0; i < u_NumPointLights; ++i) {
        vec3 toLight = u_PointLights[i].position - v_WorldPos;
        float dist = length(toLight);
        float window = clamp(1.0 - pow(dist / max(u_PointLights[i].range, 0.01), 4.0), 0.0, 1.0);
        float atten = window * window / (dist * dist + 0.01);
        Lo += BRDF(N, V, normalize(toLight), albedo, metallic, roughness, u_PointLights[i].color * atten);
    }

    // Ambient: image-based when an HDRI is loaded, constant otherwise.
    vec3 ambient;
    if (u_HasEnv == 1) {
        vec3 F0 = mix(vec3(0.04), albedo, metallic);
        vec3 kD = (1.0 - metallic) * (vec3(1.0) - FresnelSchlick(max(dot(N, V), 0.0), F0));
        vec3 irradiance = texture(u_EnvIrradiance, dirToEnvUV(N)).rgb;
        vec3 R = reflect(-V, N);
        vec3 prefiltered = textureLod(u_EnvPrefiltered, dirToEnvUV(R), roughness * 5.0).rgb;
        vec3 specular = prefiltered * EnvBRDFApprox(F0, roughness, max(dot(N, V), 0.0));
        ambient = (kD * irradiance * albedo + specular) * u_EnvIntensity;
    } else {
        ambient = 0.10 * albedo;
    }

    // Linear HDR out — the post stack tonemaps and gamma-encodes.
    FragColor = vec4(ambient + Lo + u_Emissive, 1.0);
}
