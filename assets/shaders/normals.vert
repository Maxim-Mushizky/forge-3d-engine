#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;

uniform mat4 u_ViewProj;
uniform mat4 u_Model;
uniform mat3 u_NormalMatrix;

out vec3 v_Normal;

void main()
{
    v_Normal = normalize(u_NormalMatrix * a_Normal);
    gl_Position = u_ViewProj * u_Model * vec4(a_Position, 1.0);
}
