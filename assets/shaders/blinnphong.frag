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
uniform vec3 u_Emissive; // premultiplied by strength
uniform sampler2D u_AlbedoMap;
uniform int u_HasAlbedoMap;

uniform sampler2D u_ShadowMap;
uniform int u_ShadowsEnabled;

out vec4 FragColor;

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

void main()
{
    vec3 albedo = u_Albedo;
    if (u_HasAlbedoMap == 1)
        albedo *= texture(u_AlbedoMap, v_UV).rgb;

    vec3 N = normalize(v_Normal);
    vec3 L = normalize(-u_LightDir);
    vec3 V = normalize(u_CamPos - v_WorldPos);
    vec3 H = normalize(L + V);

    float shadow = ShadowFactor(N, L);
    vec3 ambient = 0.10 * albedo;
    float ndl = max(dot(N, L), 0.0);
    vec3 diffuse = ndl * albedo * u_LightColor * u_LightIntensity;
    float spec = pow(max(dot(N, H), 0.0), 64.0) * 0.35 * ndl;

    vec3 color = ambient + (diffuse + spec * u_LightColor * u_LightIntensity) * shadow;

    for (int i = 0; i < u_NumPointLights; ++i) {
        vec3 toLight = u_PointLights[i].position - v_WorldPos;
        float dist = length(toLight);
        float window = clamp(1.0 - pow(dist / max(u_PointLights[i].range, 0.01), 4.0), 0.0, 1.0);
        float atten = window * window / (dist * dist + 0.01);
        vec3 Lp = normalize(toLight);
        vec3 Hp = normalize(Lp + V);
        float ndlp = max(dot(N, Lp), 0.0);
        color += (ndlp * albedo + pow(max(dot(N, Hp), 0.0), 64.0) * 0.35 * ndlp) * u_PointLights[i].color * atten;
    }

    // Linear HDR out — the post stack tonemaps and gamma-encodes.
    FragColor = vec4(color + u_Emissive, 1.0);
}
