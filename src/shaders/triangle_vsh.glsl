// triangle_vsh.glsl -- the stage 3 test's vertex shader: a position and a colour per vertex.
#version 460

layout (location = 0) in vec2 inPosition;
layout (location = 1) in vec3 inColor;

layout (location = 0) out vec3 outColor;

void main()
{
    gl_Position = vec4(inPosition, 0.0, 1.0);
    outColor = inColor;
}
