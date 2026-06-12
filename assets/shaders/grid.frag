#version 460 core

in vec3 v_WorldPos;

uniform vec3 u_CamPos;
uniform mat4 u_LightSpace;
uniform sampler2D u_ShadowMap;
uniform int u_ShadowsEnabled;

out vec4 FragColor;

// Anti-aliased line factor for a grid of the given cell size.
float gridFactor(vec2 p, float cellSize)
{
    vec2 coord = p / cellSize;
    vec2 d = fwidth(coord);
    vec2 g = abs(fract(coord - 0.5) - 0.5) / d;
    return 1.0 - min(min(g.x, g.y), 1.0);
}

// Shadow catcher: how shadowed is this ground point (0 = fully shadowed).
float groundShadow()
{
    if (u_ShadowsEnabled == 0)
        return 1.0;
    vec4 lightSpacePos = u_LightSpace * vec4(v_WorldPos, 1.0);
    vec3 proj = lightSpacePos.xyz / lightSpacePos.w * 0.5 + 0.5;
    if (proj.z > 1.0 || any(lessThan(proj.xy, vec2(0.0))) || any(greaterThan(proj.xy, vec2(1.0))))
        return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(u_ShadowMap, 0));
    float lit = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y) {
            float depth = texture(u_ShadowMap, proj.xy + vec2(x, y) * texel).r;
            lit += proj.z - 0.0015 > depth ? 0.0 : 1.0;
        }
    return lit / 9.0;
}

void main()
{
    float fine = gridFactor(v_WorldPos.xz, 1.0);
    float coarse = gridFactor(v_WorldPos.xz, 10.0);

    vec3 color = vec3(0.32);
    float alpha = fine * 0.35 + coarse * 0.55;

    // Axis highlights: X axis red, Z axis blue.
    vec2 dxz = fwidth(v_WorldPos.xz);
    if (abs(v_WorldPos.z) < dxz.y) { color = vec3(0.85, 0.25, 0.25); alpha = max(alpha, 0.9); }
    if (abs(v_WorldPos.x) < dxz.x) { color = vec3(0.25, 0.45, 0.95); alpha = max(alpha, 0.9); }

    // Fade with distance from the camera so the plane edge is never visible.
    float dist = length(v_WorldPos.xz - u_CamPos.xz);
    float fade = 1.0 - smoothstep(60.0, 140.0, dist);
    alpha *= fade;

    // Composite the shadow layer under the grid lines.
    float shadowAlpha = (1.0 - groundShadow()) * 0.45 * fade;
    float outAlpha = alpha + shadowAlpha * (1.0 - alpha);
    if (outAlpha < 0.01)
        discard;
    vec3 outColor = (color * alpha + vec3(0.02) * shadowAlpha * (1.0 - alpha)) / outAlpha;
    FragColor = vec4(outColor, outAlpha);
}
