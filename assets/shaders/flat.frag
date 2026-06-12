#version 460 core

in vec2 v_UV;

uniform vec3 u_Color;
uniform sampler2D u_AlbedoMap;
uniform int u_HasAlbedoMap;

out vec4 FragColor;

void main()
{
    vec3 color = u_Color;
    if (u_HasAlbedoMap == 1)
        color *= texture(u_AlbedoMap, v_UV).rgb;
    FragColor = vec4(color, 1.0);
}
