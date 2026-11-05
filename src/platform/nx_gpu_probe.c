/* nx_gpu_probe.c -- stage 3: deko3D on this console, with shaders built by uam.
 *
 * The renderer will be a deko3D backend under ps3recomp's GPU layer, with the game's shaders
 * compiled ahead of time by uam (docs/engine.md). This checks what that stands on: a deko3D
 * device and queue, a swapchain on the default window, two shader programs uam compiled at
 * build time (src/shaders, embedded in the NRO), and two seconds of frames with a triangle,
 * timed. Everything is destroyed again, so the text console can take the window after it.
 */

#include <stddef.h>
#include <string.h>

#include <deko3d.h>
#include <switch.h>

#include "triangle_fsh_dksh.h"
#include "triangle_vsh_dksh.h"

#include "nx_gpu.h"
#include "nx_log.h"

#define WIDTH       1280
#define HEIGHT      720
#define IMAGES      2
#define FRAMES      120
#define CMD_SIZE    0x10000

typedef struct {
    float position[2];
    float color[3];
} Vertex;

static const Vertex triangle[3] = {
    {{0.0f, 0.75f}, {0.85f, 0.25f, 0.20f}},
    {{-0.65f, -0.55f}, {0.20f, 0.75f, 0.30f}},
    {{0.65f, -0.55f}, {0.20f, 0.40f, 0.90f}},
};

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void nx_gpu_probe(NxGpuProbe *out)
{
    memset(out, 0, sizeof *out);

    DkDeviceMaker device_maker;
    dkDeviceMakerDefaults(&device_maker);
    DkDevice device = dkDeviceCreate(&device_maker);
    if (!device) {
        out->failed = "creating the deko3D device";
        nx_log("gpu: %s failed\n", out->failed);
        return;
    }

    /* two images to draw into and show, in GPU memory */
    DkImageLayoutMaker layout_maker;
    dkImageLayoutMakerDefaults(&layout_maker, device);
    layout_maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    layout_maker.format = DkImageFormat_RGBA8_Unorm;
    layout_maker.dimensions[0] = WIDTH;
    layout_maker.dimensions[1] = HEIGHT;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &layout_maker);
    const uint32_t image_size = align_up((uint32_t)dkImageLayoutGetSize(&layout), dkImageLayoutGetAlignment(&layout));

    DkMemBlockMaker block_maker;
    dkMemBlockMakerDefaults(&block_maker, device, align_up(IMAGES * image_size, DK_MEMBLOCK_ALIGNMENT));
    block_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    DkMemBlock image_memory = dkMemBlockCreate(&block_maker);
    DkImage images[IMAGES];
    DkImage const *image_list[IMAGES];
    for (int i = 0; i < IMAGES; i++) {
        dkImageInitialize(&images[i], &layout, image_memory, i * image_size);
        image_list[i] = &images[i];
    }
    DkSwapchainMaker swapchain_maker;
    dkSwapchainMakerDefaults(&swapchain_maker, device, nwindowGetDefault(), image_list, IMAGES);
    DkSwapchain swapchain = dkSwapchainCreate(&swapchain_maker);

    /* the shader programs uam compiled, copied to code memory */
    const uint32_t vertex_size = triangle_vsh_dksh_size, fragment_size = triangle_fsh_dksh_size;
    const uint32_t fragment_at = align_up(vertex_size, DK_SHADER_CODE_ALIGNMENT);
    dkMemBlockMakerDefaults(&block_maker, device,
                            align_up(fragment_at + fragment_size + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT));
    block_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    DkMemBlock code_memory = dkMemBlockCreate(&block_maker);
    uint8_t *code = dkMemBlockGetCpuAddr(code_memory);
    memcpy(code, triangle_vsh_dksh, vertex_size);
    memcpy(code + fragment_at, triangle_fsh_dksh, fragment_size);
    DkShader shaders[2];
    DkShaderMaker shader_maker;
    dkShaderMakerDefaults(&shader_maker, code_memory, 0);
    dkShaderInitialize(&shaders[0], &shader_maker);
    dkShaderMakerDefaults(&shader_maker, code_memory, fragment_at);
    dkShaderInitialize(&shaders[1], &shader_maker);
    out->shaders_ok = dkShaderIsValid(&shaders[0]) && dkShaderGetStage(&shaders[0]) == DkStage_Vertex &&
                      dkShaderIsValid(&shaders[1]) && dkShaderGetStage(&shaders[1]) == DkStage_Fragment;

    /* command memory, then the vertices, in one block the CPU writes */
    dkMemBlockMakerDefaults(&block_maker, device, align_up(CMD_SIZE + sizeof triangle, DK_MEMBLOCK_ALIGNMENT));
    DkMemBlock data_memory = dkMemBlockCreate(&block_maker);
    memcpy((uint8_t *)dkMemBlockGetCpuAddr(data_memory) + CMD_SIZE, triangle, sizeof triangle);

    DkCmdBufMaker cmdbuf_maker;
    dkCmdBufMakerDefaults(&cmdbuf_maker, device);
    DkCmdBuf cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
    dkCmdBufAddMemory(cmdbuf, data_memory, 0, CMD_SIZE);
    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, device);
    queue_maker.flags = DkQueueFlags_Graphics;
    DkQueue queue = dkQueueCreate(&queue_maker);

    /* one command list per image, recorded once */
    DkCmdList lists[IMAGES];
    DkShader const *bound[2] = {&shaders[0], &shaders[1]};
    const DkViewport viewport = {0.0f, 0.0f, (float)WIDTH, (float)HEIGHT, 0.0f, 1.0f};
    const DkScissor scissor = {0, 0, WIDTH, HEIGHT};
    DkRasterizerState rasterizer;
    dkRasterizerStateDefaults(&rasterizer);
    rasterizer.cullMode = DkFace_None;
    DkColorState color;
    dkColorStateDefaults(&color);
    DkColorWriteState color_write;
    dkColorWriteStateDefaults(&color_write);
    const DkVtxAttribState attributes[2] = {
        {.bufferId = 0, .isFixed = 0, .offset = offsetof(Vertex, position), .size = DkVtxAttribSize_2x32,
         .type = DkVtxAttribType_Float, .isBgra = 0},
        {.bufferId = 0, .isFixed = 0, .offset = offsetof(Vertex, color), .size = DkVtxAttribSize_3x32,
         .type = DkVtxAttribType_Float, .isBgra = 0},
    };
    const DkVtxBufferState buffer_state = {sizeof(Vertex), 0};
    for (int i = 0; i < IMAGES; i++) {
        DkImageView view;
        dkImageViewDefaults(&view, &images[i]);
        dkCmdBufBindRenderTarget(cmdbuf, &view, NULL);
        dkCmdBufSetViewports(cmdbuf, 0, &viewport, 1);
        dkCmdBufSetScissors(cmdbuf, 0, &scissor, 1);
        dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, 0.04f, 0.05f, 0.09f, 1.0f);
        dkCmdBufBindShaders(cmdbuf, DkStageFlag_GraphicsMask, bound, 2);
        dkCmdBufBindRasterizerState(cmdbuf, &rasterizer);
        dkCmdBufBindColorState(cmdbuf, &color);
        dkCmdBufBindColorWriteState(cmdbuf, &color_write);
        dkCmdBufBindVtxBuffer(cmdbuf, 0, dkMemBlockGetGpuAddr(data_memory) + CMD_SIZE, sizeof triangle);
        dkCmdBufBindVtxAttribState(cmdbuf, attributes, 2);
        dkCmdBufBindVtxBufferState(cmdbuf, &buffer_state, 1);
        dkCmdBufDraw(cmdbuf, DkPrimitive_Triangles, 3, 1, 0, 0);
        lists[i] = dkCmdBufFinishList(cmdbuf);
    }

    /* two seconds of frames, at the display's pace */
    const uint64_t start = armGetSystemTick();
    uint64_t last = start, worst = 0;
    for (int frame = 0; frame < FRAMES && out->shaders_ok; frame++) {
        const int slot = dkQueueAcquireImage(queue, swapchain);
        dkQueueSubmitCommands(queue, lists[slot]);
        dkQueuePresentImage(queue, swapchain, slot);
        const uint64_t now = armGetSystemTick();
        if (frame > 0 && now - last > worst)
            worst = now - last;
        last = now;
        out->frames++;
    }
    dkQueueWaitIdle(queue);
    if (out->frames) {
        out->frame_us = (uint32_t)(armTicksToNs(last - start) / 1000 / out->frames);
        out->worst_frame_us = (uint32_t)(armTicksToNs(worst) / 1000);
    }
    out->queue_ok = !dkQueueIsInErrorState(queue);

    dkQueueDestroy(queue);
    dkCmdBufDestroy(cmdbuf);
    dkSwapchainDestroy(swapchain);
    dkMemBlockDestroy(data_memory);
    dkMemBlockDestroy(code_memory);
    dkMemBlockDestroy(image_memory);
    dkDeviceDestroy(device);

    if (!out->shaders_ok)
        out->failed = "loading the shaders uam built";
    else if (!out->queue_ok)
        out->failed = "drawing: the queue is in an error state";
    nx_log("gpu: deko3D device, swapchain %dx%d, shaders from uam %s\n", WIDTH, HEIGHT, out->shaders_ok ? "load" : "do NOT load");
    nx_log("gpu: %u frames of a triangle, %u us each, the slowest %u us; %s\n", out->frames, out->frame_us,
           out->worst_frame_us, out->failed ? out->failed : "works");
}
