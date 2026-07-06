#version 460 core

layout(location = 0) in vec3 a_Position;

uniform mat4 u_LightSpace;
uniform mat4 u_Model;
// Section cut plane (#114): matches the main pass so the removed half stops
// casting shadows. Inert until GL_CLIP_DISTANCE0 is enabled.
uniform vec4 u_SectionPlane;

out float gl_ClipDistance[1];

void main()
{
    vec4 worldPos = u_Model * vec4(a_Position, 1.0);
    gl_ClipDistance[0] = dot(worldPos, u_SectionPlane);
    gl_Position = u_LightSpace * worldPos;
}
