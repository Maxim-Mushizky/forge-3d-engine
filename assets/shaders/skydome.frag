#version 460 core

in vec3 v_Dir;

uniform sampler2D u_Env;
uniform float u_Intensity;
uniform float u_Rotation; // radians

out vec4 FragColor;

const float PI = 3.14159265359;

void main()
{
    vec3 d = normalize(v_Dir);
    vec2 uv = vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5 + u_Rotation / (2.0 * PI),
                   acos(clamp(d.y, -1.0, 1.0)) / PI);
    FragColor = vec4(texture(u_Env, uv).rgb * u_Intensity, 1.0);
}
