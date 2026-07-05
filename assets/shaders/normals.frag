#version 460 core

in vec3 v_Normal;

out vec4 FragColor;

void main()
{
    // World-space normal remapped to color: the diagnostic view for shading
    // discontinuities and inverted faces (#93).
    FragColor = vec4(normalize(v_Normal) * 0.5 + 0.5, 1.0);
}
