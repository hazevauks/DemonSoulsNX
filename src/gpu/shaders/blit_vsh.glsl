// blit_vsh.glsl -- the core's present blit, "blit_vs" (rsxc_hdr.inc's blit_init), for deko3D:
// a full-screen triangle from the vertex index, uv 0 at the top left, as the HLSL has it.
#version 460

layout (location = 0) out vec2 outUv;

void main()
{
    vec2 t = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    outUv = t;
    gl_Position = vec4(t * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
}
