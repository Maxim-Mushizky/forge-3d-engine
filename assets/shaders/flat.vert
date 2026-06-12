#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_UV;

uniform mat4 u_ViewProj;
uniform mat4 u_Model;

out vec2 v_UV;

void main()
{
    v_UV = a_UV;
    gl_Position = u_ViewProj * u_Model * vec4(a_Position, 1.0);
}
