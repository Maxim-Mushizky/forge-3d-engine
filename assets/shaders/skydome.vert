#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_UV;

uniform mat4 u_ViewProj;
uniform vec3 u_CamPos;

out vec3 v_Dir;

void main()
{
    v_Dir = a_Position; // unit-ish sphere centered at origin -> direction
    vec4 p = u_ViewProj * vec4(a_Position * 100.0 + u_CamPos, 1.0);
    gl_Position = p.xyww; // depth = 1.0: sky fills only background pixels (LEQUAL)
}
