#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec2 a_UV;

uniform mat4 u_ViewProj;
uniform mat4 u_Model;
uniform mat3 u_NormalMatrix;
uniform mat4 u_LightSpace;

out vec3 v_WorldPos;
out vec3 v_Normal;
out vec2 v_UV;
out vec4 v_LightSpacePos;

void main()
{
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    v_WorldPos = worldPos.xyz;
    v_Normal = u_NormalMatrix * a_Normal;
    v_UV = a_UV;
    v_LightSpacePos = u_LightSpace * worldPos;
    gl_Position = u_ViewProj * worldPos;
}
