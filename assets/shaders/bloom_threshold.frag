#version 460 core

in vec2 v_UV;

uniform sampler2D u_Src;

out vec4 FragColor;

void main()
{
    vec3 c = texture(u_Src, v_UV).rgb;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    // Soft knee: only clearly-bright pixels bloom.
    FragColor = vec4(c * smoothstep(1.0, 2.0, luma), 1.0);
}
