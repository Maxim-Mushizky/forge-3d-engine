#version 460 core

in vec2 v_UV;

uniform sampler2D u_Scene;
uniform sampler2D u_Bloom;
uniform float u_BloomStrength;

out vec4 FragColor;

// ACES filmic fit (Narkowicz).
vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    vec3 color = texture(u_Scene, v_UV).rgb + texture(u_Bloom, v_UV).rgb * u_BloomStrength;
    color = aces(color);
    color = pow(color, vec3(1.0 / 2.2));
    FragColor = vec4(color, 1.0);
}
