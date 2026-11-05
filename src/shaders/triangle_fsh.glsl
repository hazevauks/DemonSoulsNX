// triangle_fsh.glsl -- the stage 3 test's fragment shader: the colour, as it comes.
#version 460

layout (location = 0) in vec3 inColor;

layout (location = 0) out vec4 outColor;

void main()
{
    outColor = vec4(inColor, 1.0);
}
