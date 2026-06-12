#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_UV;

uniform mat4 u_ViewProj;

out vec3 v_WorldPos;

void main()
{
    v_WorldPos = a_Position; // grid plane vertices are already in world space (y = 0)
    gl_Position = u_ViewProj * vec4(a_Position, 1.0);
}
