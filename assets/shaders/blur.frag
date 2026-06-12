#version 460 core

in vec2 v_UV;

uniform sampler2D u_Src;
uniform vec3 u_Dir; // xy = one texel step along the blur axis

out vec4 FragColor;

void main()
{
    const float w[5] = float[](0.227027, 0.194594, 0.121622, 0.054054, 0.016216);
    vec3 sum = texture(u_Src, v_UV).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
        vec2 off = u_Dir.xy * float(i) * 1.5;
        sum += texture(u_Src, v_UV + off).rgb * w[i];
        sum += texture(u_Src, v_UV - off).rgb * w[i];
    }
    FragColor = vec4(sum, 1.0);
}
