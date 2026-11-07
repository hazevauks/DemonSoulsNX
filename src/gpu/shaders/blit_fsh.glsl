// blit_fsh.glsl -- the core's present blit, "blit_ps": the game's frame at t0, through the
// layout's static linear clamp at s0, which the backend binds with it at texture binding 0
// (docs/deko3d.md).
#version 460

layout (location = 0) in vec2 inUv;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D frame;

void main()
{
    outColor = vec4(texture(frame, inUv).rgb, 1.0);
}
