/* gpu_dk.c -- the portable renderer's GPU layer on deko3D (docs/deko3d.md).
 *
 * One device, one graphics queue. Memory is deko3D's memory blocks, cut by a first-fit
 * allocator; buffers are ranges of them and a gpu_addr is the GPU address itself. Textures are
 * DkImages; table pools are runs of one device-wide image descriptor array and one sampler
 * descriptor array, which every recorder binds; a pipeline is the bound state, kept as one
 * record. Handles are indices + 1 into fixed tables, made with the device.
 *
 * Thread-safe where gpu.h says so (shaders and pipelines, made by the core's workers beside
 * the render thread): those take the device's lock. Everything else is the render thread's.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(__SWITCH__)

#include <deko3d.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "gpu.h"
#include "gpu_cmd_ops.h"
#include "gpu_shader.h"
#include "gpu_dk_cmd.h"

#define MAX_BUFFERS    16384u
#define MAX_TEXTURES   16384u
#define MAX_VIEWS      32768u
#define MAX_SAMPLERS   2048u
#define MAX_SHADERS    4096u
#define MAX_LAYOUTS    128u
#define MAX_PIPELINES  8192u
#define MAX_QPOOLS     32u
#define MAX_HEAPS      512u
#define MAX_TPOOLS     32u
#define MAX_SLOTS      16u
#define MAX_IMAGES     4u
#define FENCES         256u
#define IMAGE_DESCS    (1u << 19)  /* the device's image descriptors, 16 MB: the core's SRV pool alone takes 263,200 */
#define SAMPLER_DESCS  4096u       /* the sampler handle's 12 bits */
#define CHUNK          (32u << 20)
#define CODE_SIZE      (16u << 20)
#define CMD_MEM        (1u << 20)
#define PUSH_UBO       256u        /* a CONST32 slot's uniform buffer */
#define FREES          4096u

enum { MEM_GPU, MEM_UPLOAD, MEM_READBACK, MEM_IMAGE, MEM_KINDS };

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

/* ---------------------------------------------------------------------------
 * Memory
 * -------------------------------------------------------------------------*/

typedef struct Range { uint32_t off, size; } Range;

typedef struct Chunk {
    DkMemBlock block;
    DkGpuAddr  gpu;
    uint8_t*   cpu;
    uint32_t   size, kind, used;
    Range*     free;
    uint32_t   nfree, capfree;
} Chunk;

typedef struct Alloc { uint32_t chunk, off, size; } Alloc;   /* chunk index + 1; 0 none */

/* ---------------------------------------------------------------------------
 * Objects
 * -------------------------------------------------------------------------*/

typedef struct Buf {
    uint32_t  live, memory, use;
    uint64_t  size;
    Alloc     a;
    DkGpuAddr gpu;
    uint8_t*  cpu;
} Buf;

typedef struct Tex {
    uint32_t      live, swap;
    DkImage       image;
    Alloc         a;               /* its own memory; none when placed or a swap chain's */
    DkImageFormat fmt;
    gpu_format    format;
    uint32_t      width, height, layers, mips, samples, use;
    uint32_t      views;           /* its first view, index + 1 */
    int           depth, stencil;  /* a depth format, and one with stencil */
    uint32_t      bpb, block;      /* bytes a texel (or 4x4 block) */
} Tex;

typedef struct View {
    uint32_t      live;
    gpu_view_desc desc;
    DkImageView   view;
    uint32_t      tex, next;
} View;

typedef struct Smp {
    uint32_t            live;
    gpu_sampler_desc    desc;
    DkSamplerDescriptor sd;
} Smp;

typedef struct TPool {
    uint32_t       live, kind, count, base;
    gpu_view_desc* memo;           /* textures: what each entry holds (texture.id 0: empty) */
    uint32_t*      smemo;          /* samplers: the sampler each holds */
} TPool;

typedef struct Shd {
    uint32_t live, stage;
    DkShader shader;
} Shd;

typedef struct Lay {
    uint32_t        live;
    gpu_layout_desc desc;
    uint32_t        stages[GPU_MAX_SLOTS];   /* DkStageFlag_* that see each slot */
    uint32_t        static_desc[GPU_MAX_STATIC_SAMPLERS];   /* their sampler descriptors */
} Lay;

typedef struct Pipe {
    uint32_t            live, layout;
    DkShader const*     shaders[3];
    uint32_t            nshaders, stage_mask;
    DkRasterizerState   rast;
    DkMultisampleState  ms;
    DkColorState        color;
    DkColorWriteState   cw;
    DkBlendState        blend[4];
    DkDepthStencilState ds;
    DkVtxAttribState    attr[GPU_MAX_VERTEX_ATTRS];
    uint32_t            nattr, stride, nrt, strip_cut, depth_write;
    uint8_t             stencil_read, stencil_write;
} Pipe;

typedef struct QPool {
    uint32_t  live, type, count;
    Alloc     a;
    DkGpuAddr gpu;
} QPool;

typedef struct Heap {
    uint32_t   live;
    DkMemBlock block;
    uint64_t   size;
} Heap;

typedef struct Swap {
    uint32_t    live;
    DkSwapchain sc;
    DkMemBlock  mem;
    uint32_t    n, width, height;
    gpu_format  format;
    gpu_texture image[MAX_IMAGES];
    int         current;
    uint32_t    presents;
} Swap;

typedef struct Free { gpu_fence after; uint32_t kind, id; } Free;
enum { FREE_BUF, FREE_TEX };

/* A recorder. */
struct gpu_cmd {
    gpu_cmd_head  head;
    DkCmdBuf      cb;
    DkMemBlock    mem[8];          /* its command memory: the first, and more as it grows */
    uint32_t      nmem, used_mem;
    DkMemBlock    push;            /* the CONST32 slots' uniform buffers */
    DkGpuAddr     push_gpu;
    uint32_t      slot, open;
    DkCmdList     list;
    /* what is bound */
    uint32_t      layout;          /* index + 1 */
    uint32_t      pipeline;        /* index + 1 */
    gpu_table     table[GPU_MAX_SLOTS];
    uint32_t      tex_dirty;
    DkPrimitive   prim;
    DkImageView   rt[4], ds;
    uint32_t      nrt, has_ds;
    gpu_pass      pass;
    DkScissor     scissor;
    uint32_t      stencil_ref;
    uint32_t      barrier;         /* invalidate flags waiting for the next draw, clear or copy */
    uint32_t      desc_epoch;
    uint32_t      idx_size;        /* the bound index buffer's bytes per index */
    uint32_t      drawn;           /* targets written since the last barrier (pass_barrier) */
};

typedef struct DkDev {
    gpu_device  base;              /* first: a gpu_device* is this */
    DkDevice    dev;
    DkQueue     queue;
    Mutex       lock;
    uint32_t    lost;
    Chunk*      chunks;
    uint32_t    nchunks, capchunks;
    uint64_t    mem_used;
    /* descriptors */
    DkMemBlock  img_desc_mem, smp_desc_mem;
    DkImageDescriptor*   img_desc;
    DkSamplerDescriptor* smp_desc;
    DkGpuAddr   img_desc_gpu, smp_desc_gpu;
    uint32_t    img_desc_used, smp_desc_used;
    uint32_t    desc_epoch;
    DkImageDescriptor   empty_r8, empty_rgba8;   /* what an empty entry reads: zero */
    DkSamplerDescriptor empty_smp;
    gpu_texture dummy_r8, dummy_rgba8;
    /* shader code */
    DkMemBlock  code_mem;
    uint8_t*    code_cpu;
    uint32_t    code_used;
    /* tables */
    Buf*   buf;   uint32_t nbuf;   uint32_t* buf_free;  uint32_t nbuf_free;
    Tex*   tex;   uint32_t ntex;   uint32_t* tex_free;  uint32_t ntex_free;
    View*  view;  uint32_t nview;  uint32_t* view_free; uint32_t nview_free;
    Smp*   smp;   uint32_t nsmp;
    Shd*   shd;   uint32_t nshd;   uint32_t* shd_free;  uint32_t nshd_free;
    Lay    lay[MAX_LAYOUTS];       uint32_t nlay;
    Pipe*  pipe;  uint32_t npipe;
    QPool  qpool[MAX_QPOOLS];      uint32_t nqpool;
    Heap   heap[MAX_HEAPS];        uint32_t nheap;
    TPool  tpool[MAX_TPOOLS];      uint32_t ntpool;
    Swap   swap[2];
    struct gpu_cmd cmd[MAX_SLOTS];
    uint32_t ncmd;
    /* the timeline */
    DkFence  fence[FENCES];
    uint64_t signalled, completed;
    Free     frees[FREES];
    uint32_t nfrees;
    uint32_t submits;
} DkDev;

#define DD(d) ((DkDev*)(d))

static const gpu_vt k_vt;
static const gpu_cmd_ops k_cmd_ops;

/* ---------------------------------------------------------------------------
 * The allocator
 * -------------------------------------------------------------------------*/

static uint32_t kind_flags(uint32_t kind)
{
    switch (kind) {
    case MEM_READBACK: return DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;
    case MEM_IMAGE:    return DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    default:           return DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    }
}

static int chunk_new(DkDev* dd, uint32_t kind, uint32_t size)
{
    if (dd->nchunks == dd->capchunks) {
        const uint32_t cap = dd->capchunks ? dd->capchunks * 2 : 32;
        Chunk* c = realloc(dd->chunks, cap * sizeof *c);
        if (!c) return -1;
        dd->chunks = c;
        dd->capchunks = cap;
    }
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, dd->dev, (uint32_t)align_up(size, DK_MEMBLOCK_ALIGNMENT));
    m.flags = kind_flags(kind) | DkMemBlockFlags_ZeroFillInit;
    DkMemBlock b = dkMemBlockCreate(&m);
    if (!b) return -1;
    Chunk* c = &dd->chunks[dd->nchunks];
    memset(c, 0, sizeof *c);
    c->block = b;
    c->gpu = dkMemBlockGetGpuAddr(b);
    c->cpu = (uint8_t*)dkMemBlockGetCpuAddr(b);
    c->size = m.size;
    c->kind = kind;
    c->capfree = 16;
    c->free = malloc(c->capfree * sizeof *c->free);
    if (!c->free) { dkMemBlockDestroy(b); return -1; }
    c->free[0].off = 0;
    c->free[0].size = c->size;
    c->nfree = 1;
    return (int)dd->nchunks++;
}

static int chunk_take(Chunk* c, uint32_t size, uint32_t align, uint32_t* off)
{
    for (uint32_t i = 0; i < c->nfree; i++) {
        Range* r = &c->free[i];
        const uint32_t start = (uint32_t)align_up(r->off, align);
        if (start < r->off || start - r->off + (uint64_t)size > r->size) continue;
        const uint32_t before = start - r->off, after = r->size - before - size;
        if (before && after) {   /* split in two */
            if (c->nfree == c->capfree) {
                Range* f = realloc(c->free, c->capfree * 2 * sizeof *f);
                if (!f) return 0;
                c->free = f;
                c->capfree *= 2;
                r = &c->free[i];
            }
            memmove(&c->free[i + 2], &c->free[i + 1], (c->nfree - i - 1) * sizeof *r);
            c->free[i + 1].off = start + size;
            c->free[i + 1].size = after;
            r->size = before;
            c->nfree++;
        } else if (before) {
            r->size = before;
        } else if (after) {
            r->off = start + size;
            r->size = after;
        } else {
            memmove(r, r + 1, (c->nfree - i - 1) * sizeof *r);
            c->nfree--;
        }
        c->used += size;
        *off = start;
        return 1;
    }
    return 0;
}

static void chunk_give(Chunk* c, uint32_t off, uint32_t size)
{
    uint32_t i = 0;
    while (i < c->nfree && c->free[i].off < off) i++;
    const int prev = i > 0 && c->free[i - 1].off + c->free[i - 1].size == off;
    const int next = i < c->nfree && off + size == c->free[i].off;
    c->used -= size;
    if (prev && next) {
        c->free[i - 1].size += size + c->free[i].size;
        memmove(&c->free[i], &c->free[i + 1], (c->nfree - i - 1) * sizeof *c->free);
        c->nfree--;
    } else if (prev) {
        c->free[i - 1].size += size;
    } else if (next) {
        c->free[i].off = off;
        c->free[i].size += size;
    } else {
        if (c->nfree == c->capfree) {
            Range* f = realloc(c->free, c->capfree * 2 * sizeof *f);
            if (!f) return;   /* (the range is lost to the chunk, not to safety) */
            c->free = f;
            c->capfree *= 2;
        }
        memmove(&c->free[i + 1], &c->free[i], (c->nfree - i) * sizeof *c->free);
        c->free[i].off = off;
        c->free[i].size = size;
        c->nfree++;
    }
}

static Alloc mem_alloc(DkDev* dd, uint32_t kind, uint64_t size, uint32_t align)
{
    Alloc a = {0, 0, 0};
    if (!size) size = 1;
    size = align_up(size, 256);
    if (align < 256) align = 256;
    if (size > 0xFFFFF000u) return a;
    if (size <= CHUNK / 2)
        for (uint32_t i = 0; i < dd->nchunks; i++) {
            Chunk* c = &dd->chunks[i];
            if (c->kind != kind || c->size - c->used < size) continue;
            if (chunk_take(c, (uint32_t)size, align, &a.off)) {
                a.chunk = i + 1;
                a.size = (uint32_t)size;
                dd->mem_used += size;
                return a;
            }
        }
    const int n = chunk_new(dd, kind, size > CHUNK / 2 ? (uint32_t)align_up(size + align, DK_MEMBLOCK_ALIGNMENT) : CHUNK);
    if (n < 0) return a;
    if (chunk_take(&dd->chunks[n], (uint32_t)size, align, &a.off)) {
        a.chunk = (uint32_t)n + 1;
        a.size = (uint32_t)size;
        dd->mem_used += size;
    }
    return a;
}

static void mem_free(DkDev* dd, Alloc* a)
{
    if (!a->chunk) return;
    chunk_give(&dd->chunks[a->chunk - 1], a->off, a->size);
    dd->mem_used -= a->size;
    a->chunk = 0;
}

/* The chunk holding a GPU address, for the bytes a bind may read past it. */
static uint32_t bytes_from(DkDev* dd, DkGpuAddr addr, uint32_t cap)
{
    for (uint32_t i = 0; i < dd->nchunks; i++) {
        const Chunk* c = &dd->chunks[i];
        if (addr >= c->gpu && addr < c->gpu + c->size) {
            const uint64_t left = c->gpu + c->size - addr;
            return left < cap ? (uint32_t)left : cap;
        }
    }
    return cap;
}

/* ---------------------------------------------------------------------------
 * Formats
 * -------------------------------------------------------------------------*/

typedef struct Fmt { DkImageFormat dk; uint8_t bpb, block, depth, stencil; } Fmt;

/* use: a TYPELESS depth format is the depth image with GPU_USE_DS, a colour one without. */
static Fmt fmt_of(gpu_format f, uint32_t use)
{
    const int ds = (use & GPU_USE_DS) != 0;
    Fmt r = {DkImageFormat_None, 4, 0, 0, 0};
    switch (f) {
    case 2:  r.dk = DkImageFormat_RGBA32_Float; r.bpb = 16; break;
    case 6:  r.dk = DkImageFormat_RGB32_Float;  r.bpb = 12; break;
    case 10: r.dk = DkImageFormat_RGBA16_Float; r.bpb = 8; break;
    case 11: r.dk = DkImageFormat_RGBA16_Unorm; r.bpb = 8; break;
    case 16: r.dk = DkImageFormat_RG32_Float;   r.bpb = 8; break;
    case 19: case 20: r.dk = DkImageFormat_ZF32_X24S8; r.bpb = 8; r.depth = r.stencil = 1; break;
    case 21: case 22: r.dk = DkImageFormat_ZF32_X24S8; r.bpb = 8; r.depth = r.stencil = 1; break;
    case 24: r.dk = DkImageFormat_RGB10A2_Unorm; break;
    case 27: case 28: r.dk = DkImageFormat_RGBA8_Unorm; break;
    case 29: r.dk = DkImageFormat_RGBA8_Unorm_sRGB; break;
    case 34: r.dk = DkImageFormat_RG16_Float; break;
    case 35: r.dk = DkImageFormat_RG16_Unorm; break;
    case 39: if (ds) { r.dk = DkImageFormat_ZF32; r.depth = 1; } else r.dk = DkImageFormat_R32_Float; break;
    case 40: r.dk = DkImageFormat_ZF32; r.depth = 1; break;
    case 41: r.dk = DkImageFormat_R32_Float; break;
    case 42: r.dk = DkImageFormat_R32_Uint; break;
    case 44: case 45: case 46: case 47: r.dk = DkImageFormat_Z24S8; r.depth = r.stencil = 1; break;
    case 49: r.dk = DkImageFormat_RG8_Unorm; r.bpb = 2; break;
    case 54: r.dk = DkImageFormat_R16_Float; r.bpb = 2; break;
    case 55: r.dk = DkImageFormat_Z16; r.bpb = 2; r.depth = 1; break;
    case 56: r.dk = DkImageFormat_R16_Unorm; r.bpb = 2; break;
    case 57: r.dk = DkImageFormat_R16_Uint; r.bpb = 2; break;
    case 60: case 61: case 65: r.dk = DkImageFormat_R8_Unorm; r.bpb = 1; break;
    case 62: r.dk = DkImageFormat_R8_Uint; r.bpb = 1; break;
    case 70: case 71: r.dk = DkImageFormat_RGBA_BC1; r.bpb = 8; r.block = 1; break;
    case 73: case 74: r.dk = DkImageFormat_RGBA_BC2; r.bpb = 16; r.block = 1; break;
    case 76: case 77: r.dk = DkImageFormat_RGBA_BC3; r.bpb = 16; r.block = 1; break;
    case 85: r.dk = DkImageFormat_RGB565_Unorm; r.bpb = 2; break;
    case 86: r.dk = DkImageFormat_BGR5A1_Unorm; r.bpb = 2; break;
    case 87: r.dk = DkImageFormat_BGRA8_Unorm; break;
    case 88: r.dk = DkImageFormat_BGRX8_Unorm; break;
    case 90: case 91: r.dk = DkImageFormat_BGRA8_Unorm_sRGB; break;
    case 115: r.dk = DkImageFormat_RGBA4_Unorm; r.bpb = 2; break;
    default: break;
    }
    return r;
}

static uint32_t dk_format_support(gpu_device* d, gpu_format format, uint32_t samples)
{
    (void)d;
    const Fmt f = fmt_of(format, 0);
    const Fmt fd = fmt_of(format, GPU_USE_DS);
    if (fd.depth) return GPU_FMT_DS | GPU_FMT_SAMPLED | (samples > 1 ? GPU_FMT_MSAA : 0);
    if (f.dk == DkImageFormat_None) return 0;
    uint32_t bits = GPU_FMT_SAMPLED | GPU_FMT_FILTER;
    if (!f.block) bits |= GPU_FMT_RT | GPU_FMT_BLEND | (samples > 1 ? GPU_FMT_MSAA : 0);
    if (format == GPU_FORMAT_R8G8B8A8_UNORM || format == GPU_FORMAT_B8G8R8A8_UNORM) bits |= GPU_FMT_DISPLAY;
    return bits;
}

static DkImageSwizzle swz(uint32_t s)
{
    return s <= GPU_SWZ_A ? (DkImageSwizzle)(DkImageSwizzle_Red + s) : s == GPU_SWZ_ZERO ? DkImageSwizzle_Zero
                                                                                       : DkImageSwizzle_One;
}

/* ---------------------------------------------------------------------------
 * Handles
 * -------------------------------------------------------------------------*/

static uint32_t take(uint32_t* nused, uint32_t cap, uint32_t* free, uint32_t* nfree)
{
    if (free && *nfree) return free[--*nfree];
    if (*nused >= cap) return UINT32_MAX;
    return (*nused)++;
}

static Buf* buf_of(DkDev* dd, gpu_buffer b)
{
    return b.id && b.id <= dd->nbuf && dd->buf[b.id - 1].live ? &dd->buf[b.id - 1] : NULL;
}
static Tex* tex_of(DkDev* dd, gpu_texture t)
{
    return t.id && t.id <= dd->ntex && dd->tex[t.id - 1].live ? &dd->tex[t.id - 1] : NULL;
}
static View* view_of(DkDev* dd, gpu_view v)
{
    return v.id && v.id <= dd->nview && dd->view[v.id - 1].live ? &dd->view[v.id - 1] : NULL;
}

/* ---------------------------------------------------------------------------
 * The timeline
 * -------------------------------------------------------------------------*/

static void run_frees(DkDev* dd);

static uint64_t dk_fence_completed(gpu_device* d)
{
    DkDev* dd = DD(d);
    if (dd->lost) return UINT64_MAX;
    while (dd->completed < dd->signalled &&
           dkFenceWait(&dd->fence[(dd->completed + 1) % FENCES], 0) == DkResult_Success)
        dd->completed++;
    return dd->completed;
}

static int dk_fence_wait(gpu_device* d, gpu_fence f, uint32_t timeout_ms)
{
    DkDev* dd = DD(d);
    if (dd->lost || f <= dd->completed) return 0;
    if (f > dd->signalled) return 1;
    if (dkFenceWait(&dd->fence[f % FENCES], (int64_t)timeout_ms * 1000000) != DkResult_Success) return 1;
    dk_fence_completed(d);
    if (dd->completed < f) dd->completed = f;   /* (the ring's points pass in order) */
    return 0;
}

static gpu_fence dk_signal(gpu_device* d)
{
    DkDev* dd = DD(d);
    const uint64_t v = dd->signalled + 1;
    if (v - dd->completed >= FENCES) dk_fence_wait(d, v - FENCES + 1, 5000);   /* the ring's oldest point first */
    dkQueueSignalFence(dd->queue, &dd->fence[v % FENCES], true);
    dkQueueFlush(dd->queue);
    dd->signalled = v;
    return v;
}

static void free_now(DkDev* dd, uint32_t kind, uint32_t id)
{
    if (kind == FREE_BUF) {
        Buf* b = &dd->buf[id - 1];
        mem_free(dd, &b->a);
        memset(b, 0, sizeof *b);
        dd->buf_free[dd->nbuf_free++] = id - 1;
        return;
    }
    Tex* t = &dd->tex[id - 1];
    for (uint32_t v = t->views; v; ) {
        View* vw = &dd->view[v - 1];
        const uint32_t next = vw->next;
        memset(vw, 0, sizeof *vw);
        dd->view_free[dd->nview_free++] = v - 1;
        v = next;
    }
    mem_free(dd, &t->a);
    memset(t, 0, sizeof *t);
    dd->tex_free[dd->ntex_free++] = id - 1;
}

/* Freed once the GPU has passed `after` (gpu.h); at once when it has, or when the list is full
 * (after waiting for it). */
static void defer_free(DkDev* dd, uint32_t kind, uint32_t id, gpu_fence after)
{
    if (after && after > dk_fence_completed(&dd->base)) {
        if (dd->nfrees < FREES) {
            dd->frees[dd->nfrees].after = after;
            dd->frees[dd->nfrees].kind = kind;
            dd->frees[dd->nfrees].id = id;
            dd->nfrees++;
            return;
        }
        dk_fence_wait(&dd->base, after, 5000);
    }
    free_now(dd, kind, id);
}

static void run_frees(DkDev* dd)
{
    const uint64_t done = dk_fence_completed(&dd->base);
    uint32_t keep = 0;
    for (uint32_t i = 0; i < dd->nfrees; i++) {
        const Free f = dd->frees[i];
        if (f.after <= done) free_now(dd, f.kind, f.id);
        else dd->frees[keep++] = f;
    }
    dd->nfrees = keep;
}

/* ---------------------------------------------------------------------------
 * The device
 * -------------------------------------------------------------------------*/

static void dk_destroy(gpu_device* d)
{
    DkDev* dd = DD(d);
    dkQueueWaitIdle(dd->queue);
    for (uint32_t i = 0; i < 2; i++)
        if (dd->swap[i].live) dkSwapchainDestroy(dd->swap[i].sc);
    for (uint32_t i = 0; i < dd->ncmd; i++) {
        dkCmdBufDestroy(dd->cmd[i].cb);
        for (uint32_t m = 0; m < dd->cmd[i].nmem; m++) dkMemBlockDestroy(dd->cmd[i].mem[m]);
        if (dd->cmd[i].push) dkMemBlockDestroy(dd->cmd[i].push);
    }
    dkQueueDestroy(dd->queue);
    for (uint32_t i = 0; i < 2; i++)
        if (dd->swap[i].mem) dkMemBlockDestroy(dd->swap[i].mem);
    for (uint32_t i = 0; i < dd->nheap; i++)
        if (dd->heap[i].block) dkMemBlockDestroy(dd->heap[i].block);
    for (uint32_t i = 0; i < dd->nchunks; i++) {
        dkMemBlockDestroy(dd->chunks[i].block);
        free(dd->chunks[i].free);
    }
    for (uint32_t i = 0; i < dd->ntpool; i++) {
        free(dd->tpool[i].memo);
        free(dd->tpool[i].smemo);
    }
    dkMemBlockDestroy(dd->code_mem);
    dkMemBlockDestroy(dd->img_desc_mem);
    dkMemBlockDestroy(dd->smp_desc_mem);
    dkDeviceDestroy(dd->dev);
    free(dd->chunks);
    free(dd->buf); free(dd->buf_free);
    free(dd->tex); free(dd->tex_free);
    free(dd->view); free(dd->view_free);
    free(dd->smp); free(dd->shd); free(dd->shd_free); free(dd->pipe);
    free(dd);
}

static void dk_adapter(gpu_device* d, gpu_adapter_info* out)
{
    (void)d;
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "NVIDIA Tegra X1 (Nintendo Switch)");
    snprintf(out->driver, sizeof out->driver, "deko3D");
    out->vendor = 0x10DE;
}

static uint32_t dk_lost(gpu_device* d)
{
    DkDev* dd = DD(d);
    if (!dd->lost && dkQueueIsInErrorState(dd->queue)) {
        dd->lost = GPU_DK_LOST_QUEUE;
        printf("[dk] the GPU queue is in its error state: the device is lost\n");
    }
    return dd->lost;
}

void gpu_dk_lose(gpu_device* d) { DD(d)->lost = GPU_DK_LOST_QUEUE; }

static void dk_wait_idle(gpu_device* d)
{
    DkDev* dd = DD(d);
    dkQueueWaitIdle(dd->queue);
    dd->completed = dd->signalled;
    run_frees(dd);
}

/* ---------------------------------------------------------------------------
 * Buffers
 * -------------------------------------------------------------------------*/

static gpu_buffer dk_buffer_create(gpu_device* d, const gpu_buffer_desc* desc)
{
    DkDev* dd = DD(d);
    gpu_buffer none = {0};
    const uint32_t kind = desc->memory == GPU_MEM_READBACK ? MEM_READBACK
                          : desc->memory == GPU_MEM_UPLOAD ? MEM_UPLOAD : MEM_GPU;
    const uint32_t i = take(&dd->nbuf, MAX_BUFFERS, dd->buf_free, &dd->nbuf_free);
    if (i == UINT32_MAX) return none;
    Buf* b = &dd->buf[i];
    memset(b, 0, sizeof *b);
    b->a = mem_alloc(dd, kind, desc->size, 256);
    if (!b->a.chunk) {
        dd->buf_free[dd->nbuf_free++] = i;
        printf("[dk] buffer_create: no memory for %llu bytes (%s)\n", (unsigned long long)desc->size,
               desc->name ? desc->name : "?");
        return none;
    }
    const Chunk* c = &dd->chunks[b->a.chunk - 1];
    b->live = 1;
    b->memory = desc->memory;
    b->use = desc->use;
    b->size = desc->size;
    b->gpu = c->gpu + b->a.off;
    b->cpu = c->cpu + b->a.off;
    gpu_buffer h = {i + 1};
    return h;
}

static gpu_buffer dk_buffer_import_host(gpu_device* d, void* host, uint64_t size)
{
    (void)d; (void)host; (void)size;
    gpu_buffer none = {0};
    return none;   /* (no GPU_CAP_HOST_IMPORT: the guest window is not memory nvmap takes) */
}

static void dk_buffer_destroy(gpu_device* d, gpu_buffer b, gpu_fence after)
{
    DkDev* dd = DD(d);
    if (buf_of(dd, b)) defer_free(dd, FREE_BUF, b.id, after);
}

static void* dk_buffer_map(gpu_device* d, gpu_buffer b, uint64_t read_offset, uint64_t read_size)
{
    DkDev* dd = DD(d);
    Buf* bb = buf_of(dd, b);
    if (!bb) return NULL;
    if (bb->memory == GPU_MEM_READBACK && read_size) {
        const Chunk* c = &dd->chunks[bb->a.chunk - 1];
        dkMemBlockFlushCpuCache(c->block, bb->a.off + (uint32_t)read_offset, (uint32_t)read_size);
    }
    return bb->cpu;
}

static void dk_buffer_unmap(gpu_device* d, gpu_buffer b, uint64_t written_offset, uint64_t written_size)
{
    (void)d; (void)b; (void)written_offset; (void)written_size;   /* (upload memory is CPU-uncached) */
}

static gpu_addr dk_buffer_addr(gpu_device* d, gpu_buffer b)
{
    const Buf* bb = buf_of(DD(d), b);
    return bb ? bb->gpu : 0;
}

/* ---------------------------------------------------------------------------
 * Textures
 * -------------------------------------------------------------------------*/

static void layout_maker(DkDev* dd, const gpu_texture_desc* desc, const Fmt* f, DkImageLayoutMaker* m, uint32_t extra)
{
    dkImageLayoutMakerDefaults(m, dd->dev);
    const uint32_t layers = desc->layers ? desc->layers : 1;
    const uint32_t samples = desc->samples ? desc->samples : 1;
    m->type = samples > 1 ? (layers > 1 ? DkImageType_2DMSArray : DkImageType_2DMS)
                          : (layers > 1 ? DkImageType_2DArray : DkImageType_2D);
    m->format = f->dk;
    m->msMode = samples >= 8 ? DkMsMode_8x : samples >= 4 ? DkMsMode_4x : samples >= 2 ? DkMsMode_2x : DkMsMode_1x;
    m->dimensions[0] = desc->width ? desc->width : 1;
    m->dimensions[1] = desc->height ? desc->height : 1;
    m->dimensions[2] = layers;
    m->mipLevels = desc->mips ? desc->mips : 1;
    m->flags = extra;
    if (desc->use & (GPU_USE_RT | GPU_USE_DS)) m->flags |= DkImageFlags_UsageRender | DkImageFlags_HwCompression;
    if (!f->block && !f->depth) m->flags |= DkImageFlags_Usage2DEngine;
}

static gpu_heap dk_heap_create(gpu_device* d, const gpu_heap_desc* desc)
{
    DkDev* dd = DD(d);
    gpu_heap none = {0};
    if (dd->nheap >= MAX_HEAPS) return none;
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, dd->dev, (uint32_t)align_up(desc->size, DK_MEMBLOCK_ALIGNMENT));
    m.flags = kind_flags(MEM_IMAGE);
    DkMemBlock b = dkMemBlockCreate(&m);
    if (!b) return none;
    Heap* h = &dd->heap[dd->nheap];
    h->live = 1;
    h->block = b;
    h->size = desc->size;
    dd->mem_used += desc->size;
    gpu_heap r = {++dd->nheap};
    return r;
}

static void dk_texture_size(gpu_device* d, const gpu_texture_desc* desc, uint64_t* size, uint64_t* align)
{
    DkDev* dd = DD(d);
    const Fmt f = fmt_of(desc->format, desc->use);
    DkImageLayoutMaker m;
    layout_maker(dd, desc, &f, &m, 0);
    DkImageLayout l;
    dkImageLayoutInitialize(&l, &m);
    if (size) *size = dkImageLayoutGetSize(&l);
    if (align) *align = dkImageLayoutGetAlignment(&l);
}

/* GetCopyableFootprints as D3D12 lays out the formats (the null backend's arithmetic): rows of
 * row_pitch, a multiple of 256, from offsets that are multiples of 512; whole blocks for BC;
 * a planar depth's planes as R32 and R8. */
static uint64_t dk_texture_footprints(gpu_device* d, const gpu_texture_desc* desc, uint32_t first_sub, uint32_t n,
                                      uint64_t base_offset, gpu_footprint* out, uint32_t* rows, uint64_t* row_bytes)
{
    (void)d;
    const uint32_t mips = desc->mips ? desc->mips : 1u, layers = desc->layers ? desc->layers : 1u;
    const Fmt df = fmt_of(desc->format, GPU_USE_DS);
    const int planar = df.depth && df.stencil;
    uint64_t off = base_offset, end = base_offset;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t s = first_sub + i, mip = s % mips, plane = s / (mips * layers);
        uint32_t w = desc->width >> mip, h = desc->height >> mip;
        if (!w) w = 1;
        if (!h) h = 1;
        gpu_format pf = desc->format;
        uint32_t bytes;
        int block = 0;
        if (planar) {
            pf = plane ? (gpu_format)60 : GPU_FORMAT_R32_TYPELESS;
            bytes = plane ? 1 : 4;
        } else {
            const Fmt f = fmt_of(desc->format, desc->use);
            bytes = f.bpb ? f.bpb : 4;
            block = f.block;
        }
        uint32_t nrows = h;
        uint64_t rb = (uint64_t)w * bytes;
        if (block) {
            w = (w + 3) & ~3u;
            h = (h + 3) & ~3u;
            nrows = h / 4;
            rb = (uint64_t)(w / 4) * bytes;
        }
        const uint64_t pitch = align_up(rb, 256);
        off = align_up(off, 512);
        if (out) {
            out[i].offset = off;
            out[i].format = pf;
            out[i].width = w;
            out[i].height = h;
            out[i].depth = 1;
            out[i].row_pitch = (uint32_t)pitch;
        }
        if (rows) rows[i] = nrows;
        if (row_bytes) row_bytes[i] = rb;
        end = off + (uint64_t)(nrows - 1) * pitch + rb;
        off += (uint64_t)nrows * pitch;
    }
    return n ? end - base_offset : 0;
}

static gpu_texture dk_texture_create(gpu_device* d, const gpu_texture_desc* desc)
{
    DkDev* dd = DD(d);
    gpu_texture none = {0};
    const Fmt f = fmt_of(desc->format, desc->use);
    if (f.dk == DkImageFormat_None) {
        printf("[dk] texture_create: format %u is not mapped (%s)\n", desc->format, desc->name ? desc->name : "?");
        return none;
    }
    const uint32_t i = take(&dd->ntex, MAX_TEXTURES, dd->tex_free, &dd->ntex_free);
    if (i == UINT32_MAX) return none;
    Tex* t = &dd->tex[i];
    memset(t, 0, sizeof *t);
    DkImageLayoutMaker m;
    layout_maker(dd, desc, &f, &m, 0);
    DkImageLayout l;
    dkImageLayoutInitialize(&l, &m);
    const uint64_t size = dkImageLayoutGetSize(&l);
    const uint32_t align = dkImageLayoutGetAlignment(&l);
    if (desc->heap.id) {
        if (desc->heap.id > dd->nheap || desc->heap_offset + size > dd->heap[desc->heap.id - 1].size) {
            dd->tex_free[dd->ntex_free++] = i;
            return none;
        }
        dkImageInitialize(&t->image, &l, dd->heap[desc->heap.id - 1].block, (uint32_t)desc->heap_offset);
    } else {
        t->a = mem_alloc(dd, MEM_IMAGE, size, align);
        if (!t->a.chunk) {
            dd->tex_free[dd->ntex_free++] = i;
            printf("[dk] texture_create: no memory for %ux%u (%llu bytes, %s)\n", desc->width, desc->height,
                   (unsigned long long)size, desc->name ? desc->name : "?");
            return none;
        }
        dkImageInitialize(&t->image, &l, dd->chunks[t->a.chunk - 1].block, t->a.off);
    }
    t->live = 1;
    t->fmt = f.dk;
    t->format = desc->format;
    t->width = desc->width;
    t->height = desc->height;
    t->layers = desc->layers ? desc->layers : 1;
    t->mips = desc->mips ? desc->mips : 1;
    t->samples = desc->samples ? desc->samples : 1;
    t->use = desc->use;
    t->depth = f.depth;
    t->stencil = f.stencil;
    t->bpb = f.bpb;
    t->block = f.block;
    gpu_texture h = {i + 1};
    return h;
}

static void dk_texture_destroy(gpu_device* d, gpu_texture t, gpu_fence after)
{
    DkDev* dd = DD(d);
    Tex* tt = tex_of(dd, t);
    if (!tt || tt->swap) return;
    defer_free(dd, FREE_TEX, t.id, after);
}

/* A DkImageView of a texture, for a view description (target, texture, or a copy's subresource). */
static void make_view(const Tex* t, const gpu_view_desc* v, DkImageView* out)
{
    dkImageViewDefaults(out, &t->image);
    if (v->format && v->format != t->format) {
        const Fmt f = fmt_of(v->format, v->kind == GPU_VIEW_DSV ? GPU_USE_DS : 0);
        if (f.dk != DkImageFormat_None && f.dk != t->fmt && !f.depth) out->format = f.dk;
    }
    if (t->depth && v->kind == GPU_VIEW_SRV &&
        (v->format == 47 || v->format == 22))   /* X24_TYPELESS_G8_UINT, X32_TYPELESS_G8X24_UINT */
        out->dsSource = DkDsSource_Stencil;
    if (v->kind == GPU_VIEW_SRV) {
        if (v->format == GPU_FORMAT_A8_UNORM) {
            out->swizzle[0] = out->swizzle[1] = out->swizzle[2] = DkImageSwizzle_Zero;
            out->swizzle[3] = DkImageSwizzle_Red;
        }
        if (v->swizzle && v->swizzle != GPU_SWIZZLE_IDENTITY) {
            DkImageSwizzle base[4];
            memcpy(base, out->swizzle, sizeof base);
            for (int c = 0; c < 4; c++) {
                const uint32_t s = GPU_SWIZZLE_GET(v->swizzle, c);
                out->swizzle[c] = s <= GPU_SWZ_A ? base[s] : swz(s);
            }
        }
        switch (v->dim) {
        case GPU_DIM_CUBE: out->type = DkImageType_Cubemap; break;
        case GPU_DIM_2D_ARRAY: out->type = DkImageType_2DArray; break;
        case GPU_DIM_2DMS: out->type = DkImageType_2DMS; break;
        default: if (t->layers > 1 && t->samples == 1) { out->type = DkImageType_2D; out->layerCount = 1; } break;
        }
        out->mipLevelOffset = (uint8_t)v->mip;
        out->mipLevelCount = v->mips ? v->mips : (uint8_t)(t->mips - v->mip);
    } else {
        out->mipLevelOffset = (uint8_t)v->mip;
        out->mipLevelCount = 1;
        if (t->layers > 1) {
            out->layerOffset = v->layer;
            out->layerCount = 1;
            out->type = t->samples > 1 ? DkImageType_2DMS : DkImageType_2D;
        }
    }
}

static gpu_view dk_view_get(gpu_device* d, const gpu_view_desc* desc)
{
    DkDev* dd = DD(d);
    gpu_view none = {0};
    Tex* t = tex_of(dd, desc->texture);
    if (!t) return none;
    for (uint32_t v = t->views; v; v = dd->view[v - 1].next)
        if (!memcmp(&dd->view[v - 1].desc, desc, sizeof *desc)) {
            gpu_view h = {v};
            return h;
        }
    const uint32_t i = take(&dd->nview, MAX_VIEWS, dd->view_free, &dd->nview_free);
    if (i == UINT32_MAX) return none;
    View* vw = &dd->view[i];
    memset(vw, 0, sizeof *vw);
    vw->live = 1;
    vw->desc = *desc;
    vw->tex = desc->texture.id - 1;
    make_view(t, desc, &vw->view);
    vw->next = t->views;
    t->views = i + 1;
    gpu_view h = {i + 1};
    return h;
}

/* ---------------------------------------------------------------------------
 * Samplers and tables
 * -------------------------------------------------------------------------*/

static DkWrapMode wrap(uint32_t a)
{
    switch (a) {
    case GPU_ADDRESS_MIRROR: return DkWrapMode_MirroredRepeat;
    case GPU_ADDRESS_CLAMP: return DkWrapMode_ClampToEdge;
    case GPU_ADDRESS_BORDER: return DkWrapMode_ClampToBorder;
    case GPU_ADDRESS_MIRROR_ONCE: return DkWrapMode_MirrorClampToEdge;
    default: return DkWrapMode_Repeat;
    }
}

static void sampler_desc(DkDev* dd, const gpu_sampler_desc* s, DkSamplerDescriptor* out)
{
    (void)dd;
    DkSampler k;
    dkSamplerDefaults(&k);
    const uint32_t f = s->filter;
    const int aniso = GPU_FILTER_IS_ANISO(f);
    k.minFilter = (aniso || ((f >> 4) & 3)) ? DkFilter_Linear : DkFilter_Nearest;
    k.magFilter = (aniso || ((f >> 2) & 3)) ? DkFilter_Linear : DkFilter_Nearest;
    k.mipFilter = (aniso || (f & 3)) ? DkMipFilter_Linear : DkMipFilter_Nearest;
    k.wrapMode[0] = wrap(s->address_u);
    k.wrapMode[1] = wrap(s->address_v);
    k.wrapMode[2] = wrap(s->address_w);
    k.lodBias = s->mip_lod_bias;
    k.lodClampMin = s->min_lod;
    k.lodClampMax = s->max_lod > 1000.0f ? 1000.0f : s->max_lod;
    k.maxAnisotropy = aniso ? (float)(s->max_aniso ? s->max_aniso : 1) : 1.0f;
    if (((f >> 7) & 3) == GPU_REDUCE_COMPARE) {
        k.compareEnable = true;
        k.compareOp = (DkCompareOp)s->compare;   /* (the same numbers) */
    }
    for (int c = 0; c < 4; c++) k.borderColor[c].value_f = s->border[c];
    dkSamplerDescriptorInitialize(out, &k);
}

static gpu_sampler dk_sampler_get(gpu_device* d, const gpu_sampler_desc* desc)
{
    DkDev* dd = DD(d);
    gpu_sampler none = {0};
    for (uint32_t i = 0; i < dd->nsmp; i++)
        if (!memcmp(&dd->smp[i].desc, desc, sizeof *desc)) {
            gpu_sampler h = {i + 1};
            return h;
        }
    if (dd->nsmp >= MAX_SAMPLERS) return none;
    Smp* s = &dd->smp[dd->nsmp];
    s->live = 1;
    s->desc = *desc;
    sampler_desc(dd, desc, &s->sd);
    gpu_sampler h = {++dd->nsmp};
    return h;
}

static gpu_table_pool dk_table_pool_create(gpu_device* d, const gpu_table_pool_desc* desc)
{
    DkDev* dd = DD(d);
    gpu_table_pool none = {0};
    if (dd->ntpool >= MAX_TPOOLS || !desc->count) return none;
    TPool* p = &dd->tpool[dd->ntpool];
    memset(p, 0, sizeof *p);
    p->kind = desc->kind;
    p->count = desc->count;
    if (desc->kind == GPU_TABLE_TEX) {
        if (dd->img_desc_used + desc->count > IMAGE_DESCS) return none;
        p->base = dd->img_desc_used;
        dd->img_desc_used += desc->count;
        p->memo = calloc(desc->count, sizeof *p->memo);
        if (!p->memo) return none;
        for (uint32_t i = 0; i < desc->count; i++) dd->img_desc[p->base + i] = dd->empty_r8;
    } else {
        if (dd->smp_desc_used + desc->count > SAMPLER_DESCS) return none;
        p->base = dd->smp_desc_used;
        dd->smp_desc_used += desc->count;
        p->smemo = calloc(desc->count, sizeof *p->smemo);
        if (!p->smemo) return none;
        for (uint32_t i = 0; i < desc->count; i++) dd->smp_desc[p->base + i] = dd->empty_smp;
    }
    p->live = 1;
    dd->desc_epoch++;
    gpu_table_pool h = {++dd->ntpool};
    return h;
}

static void write_tex(DkDev* dd, TPool* p, uint32_t index, const gpu_view_desc* srv)
{
    if (!p || p->kind != GPU_TABLE_TEX || index >= p->count) return;
    gpu_view_desc* memo = &p->memo[index];
    if (srv) {
        if (memo->texture.id && gpu_view_memo_same(memo, srv, srv->swizzle ? srv->swizzle : GPU_SWIZZLE_IDENTITY))
            return;
        const Tex* t = tex_of(dd, srv->texture);
        if (!t) {
            dd->img_desc[p->base + index] = dd->empty_rgba8;
            memset(memo, 0, sizeof *memo);
        } else {
            DkImageView v;
            gpu_view_desc k = *srv;
            k.kind = GPU_VIEW_SRV;
            make_view(t, &k, &v);
            dkImageDescriptorInitialize(&dd->img_desc[p->base + index], &v, false, false);
            *memo = *srv;
            if (!memo->swizzle) memo->swizzle = GPU_SWIZZLE_IDENTITY;
        }
    } else {
        dd->img_desc[p->base + index] = dd->empty_rgba8;
        memset(memo, 0, sizeof *memo);
    }
    dd->desc_epoch++;
}

static void dk_table_write_tex(gpu_device* d, gpu_table_pool pool, uint32_t index, const gpu_view_desc* srv)
{
    DkDev* dd = DD(d);
    write_tex(dd, pool.id && pool.id <= dd->ntpool ? &dd->tpool[pool.id - 1] : NULL, index, srv);
}

static void dk_table_write_sampler(gpu_device* d, gpu_table_pool pool, uint32_t index, gpu_sampler s)
{
    DkDev* dd = DD(d);
    TPool* p = pool.id && pool.id <= dd->ntpool ? &dd->tpool[pool.id - 1] : NULL;
    if (!p || p->kind != GPU_TABLE_SAMPLER || index >= p->count) return;
    if (p->smemo[index] == s.id) return;
    dd->smp_desc[p->base + index] = s.id && s.id <= dd->nsmp ? dd->smp[s.id - 1].sd : dd->empty_smp;
    p->smemo[index] = s.id;
    dd->desc_epoch++;
}

static gpu_table_writer dk_table_writer_get(gpu_device* d, gpu_table_pool pool)
{
    (void)d;
    gpu_table_writer w = {pool.id};
    return w;
}

static void dk_table_writer_tex(gpu_device* d, gpu_table_writer w, uint32_t index, const gpu_view_desc* srv)
{
    DkDev* dd = DD(d);
    write_tex(dd, w.v && w.v <= dd->ntpool ? &dd->tpool[w.v - 1] : NULL, index, srv);
}

/* A table: its kind in bit 48, its first descriptor's index + 1 below. */
static gpu_table dk_table_ref(gpu_device* d, gpu_table_pool pool, uint32_t index)
{
    DkDev* dd = DD(d);
    gpu_table t = {0};
    if (!pool.id || pool.id > dd->ntpool) return t;
    const TPool* p = &dd->tpool[pool.id - 1];
    if (index >= p->count) return t;
    t.v = ((uint64_t)p->kind << 48) | (uint64_t)(p->base + index + 1);
    return t;
}

/* ---------------------------------------------------------------------------
 * Shaders, layouts, pipelines
 * -------------------------------------------------------------------------*/

static int dk_compile_hook(gpu_device* d, const void* request, void* result)
{
    (void)d; (void)request; (void)result;
    return 0;   /* (the instance file compiles: rsxc_dk.c's rc_be_sc_compile) */
}

static gpu_shader dk_shader_create(gpu_device* d, const gpu_shader_desc* desc)
{
    DkDev* dd = DD(d);
    gpu_shader none = {0};
    if (desc->code != GPU_CODE_DKSH || !desc->bytes || !desc->size) return none;
    mutexLock(&dd->lock);
    const uint32_t at = (uint32_t)align_up(dd->code_used, DK_SHADER_CODE_ALIGNMENT);
    const uint32_t i = take(&dd->nshd, MAX_SHADERS, dd->shd_free, &dd->nshd_free);
    if (i == UINT32_MAX || at + desc->size + DK_SHADER_CODE_UNUSABLE_SIZE > CODE_SIZE) {
        mutexUnlock(&dd->lock);
        printf("[dk] shader_create: no room for a shader of %llu bytes\n", (unsigned long long)desc->size);
        return none;
    }
    memcpy(dd->code_cpu + at, desc->bytes, desc->size);
    dd->code_used = at + (uint32_t)desc->size;
    Shd* s = &dd->shd[i];
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, dd->code_mem, at);
    dkShaderInitialize(&s->shader, &m);
    if (!dkShaderIsValid(&s->shader)) {
        dd->shd_free[dd->nshd_free++] = i;
        mutexUnlock(&dd->lock);
        printf("[dk] shader_create: the DKSH was not taken\n");
        return none;
    }
    s->live = 1;
    s->stage = desc->stage;
    mutexUnlock(&dd->lock);
    gpu_shader h = {i + 1};
    return h;
}

static void dk_shader_destroy(gpu_device* d, gpu_shader s)
{
    (void)d; (void)s;   /* (its code stays in code memory: pipelines made from it keep using it) */
}

static uint32_t vis_stages(uint32_t vis)
{
    switch (vis) {
    case GPU_VIS_VERTEX: return DkStageFlag_Vertex;
    case GPU_VIS_GEOMETRY: return DkStageFlag_Geometry;
    case GPU_VIS_PIXEL: return DkStageFlag_Fragment;
    default: return DkStageFlag_Vertex | DkStageFlag_Geometry | DkStageFlag_Fragment;
    }
}

static gpu_layout dk_layout_create(gpu_device* d, const gpu_layout_desc* desc)
{
    DkDev* dd = DD(d);
    gpu_layout none = {0};
    mutexLock(&dd->lock);
    for (uint32_t i = 0; i < dd->nlay; i++)
        if (!memcmp(&dd->lay[i].desc, desc, sizeof *desc)) {
            mutexUnlock(&dd->lock);
            gpu_layout h = {i + 1};
            return h;
        }
    if (dd->nlay >= MAX_LAYOUTS || dd->smp_desc_used + desc->nstatic > SAMPLER_DESCS) {
        mutexUnlock(&dd->lock);
        return none;
    }
    Lay* l = &dd->lay[dd->nlay];
    memset(l, 0, sizeof *l);
    l->desc = *desc;
    for (uint32_t s = 0; s < desc->nslots && s < GPU_MAX_SLOTS; s++) l->stages[s] = vis_stages(desc->slot[s].visibility);
    for (uint32_t s = 0; s < desc->nstatic && s < GPU_MAX_STATIC_SAMPLERS; s++) {
        l->static_desc[s] = dd->smp_desc_used++;
        sampler_desc(dd, &desc->statics[s].desc, &dd->smp_desc[l->static_desc[s]]);
    }
    l->live = 1;
    dd->desc_epoch++;
    gpu_layout h = {++dd->nlay};
    mutexUnlock(&dd->lock);
    return h;
}

static int vtx_format(gpu_format f, DkVtxAttribState* a)
{
    a->isBgra = 0;
    switch (f) {
    case 2:  a->size = DkVtxAttribSize_4x32; a->type = DkVtxAttribType_Float; return 1;
    case 6:  a->size = DkVtxAttribSize_3x32; a->type = DkVtxAttribType_Float; return 1;
    case 16: a->size = DkVtxAttribSize_2x32; a->type = DkVtxAttribType_Float; return 1;
    case 41: a->size = DkVtxAttribSize_1x32; a->type = DkVtxAttribType_Float; return 1;
    case 42: a->size = DkVtxAttribSize_1x32; a->type = DkVtxAttribType_Uint; return 1;
    case 10: a->size = DkVtxAttribSize_4x16; a->type = DkVtxAttribType_Float; return 1;
    case 34: a->size = DkVtxAttribSize_2x16; a->type = DkVtxAttribType_Float; return 1;
    case 28: a->size = DkVtxAttribSize_4x8; a->type = DkVtxAttribType_Unorm; return 1;
    case 87: a->size = DkVtxAttribSize_4x8; a->type = DkVtxAttribType_Unorm; a->isBgra = 1; return 1;
    case 49: a->size = DkVtxAttribSize_2x8; a->type = DkVtxAttribType_Unorm; return 1;
    default: return 0;
    }
}

static DkBlendFactor blend_factor(uint8_t f)
{
    if (f == GPU_BLEND_BLEND_FACTOR) return DkBlendFactor_ConstColor;
    if (f == GPU_BLEND_INV_BLEND_FACTOR) return DkBlendFactor_InvConstColor;
    return (DkBlendFactor)f;   /* (D3D12's other numbers are deko3D's) */
}

static gpu_pipeline dk_pipeline_create(gpu_device* d, const gpu_pipeline_desc* desc, uint32_t flags, uint32_t* how)
{
    DkDev* dd = DD(d);
    gpu_pipeline none = {0};
    if (how) *how = GPU_PIPELINE_FAILED;
    const gpu_shader sh[3] = {desc->vs, desc->gs, desc->ps};
    const uint32_t stage_of[3] = {DkStageFlag_Vertex, DkStageFlag_Geometry, DkStageFlag_Fragment};
    if (!desc->vs.id) return none;
    if (flags & GPU_PIPELINE_STORE_ONLY) {
        if (how) *how = GPU_PIPELINE_COMPILED;
        return none;
    }
    mutexLock(&dd->lock);
    if (dd->npipe >= MAX_PIPELINES) { mutexUnlock(&dd->lock); return none; }
    Pipe* p = &dd->pipe[dd->npipe];
    memset(p, 0, sizeof *p);
    for (int s = 0; s < 3; s++) {
        if (!sh[s].id) continue;
        if (sh[s].id > dd->nshd || !dd->shd[sh[s].id - 1].live) { mutexUnlock(&dd->lock); return none; }
        p->shaders[p->nshaders++] = &dd->shd[sh[s].id - 1].shader;
        p->stage_mask |= stage_of[s];
    }
    if (!(p->stage_mask & DkStageFlag_Geometry)) p->stage_mask |= DkStageFlag_Geometry;   /* (unbinds a GS left bound) */
    p->stage_mask |= DkStageFlag_Fragment;
    p->layout = desc->layout.id;

    dkRasterizerStateDefaults(&p->rast);
    p->rast.cullMode = desc->cull == GPU_CULL_FRONT ? DkFace_Front : desc->cull == GPU_CULL_BACK ? DkFace_Back : DkFace_None;
    p->rast.frontFace = desc->front_ccw ? DkFrontFace_CCW : DkFrontFace_CW;
    p->rast.depthClampEnable = desc->depth_clip ? 0 : 1;

    dkMultisampleStateDefaults(&p->ms);
    const uint32_t samples = desc->samples ? desc->samples : 1;
    p->ms.mode = samples >= 8 ? DkMsMode_8x : samples >= 4 ? DkMsMode_4x : samples >= 2 ? DkMsMode_2x : DkMsMode_1x;
    p->ms.rasterizerMode = p->ms.mode;

    dkColorStateDefaults(&p->color);
    dkColorWriteStateDefaults(&p->cw);
    p->nrt = desc->nrt > 4 ? 4 : desc->nrt;
    for (uint32_t t = 0; t < 4; t++) {
        dkBlendStateDefaults(&p->blend[t]);
        dkBlendStateSetOps(&p->blend[t], (DkBlendOp)(desc->blend.op ? desc->blend.op : 1),
                           (DkBlendOp)(desc->blend.op_alpha ? desc->blend.op_alpha : 1));
        dkBlendStateSetFactors(&p->blend[t], blend_factor(desc->blend.src ? desc->blend.src : 2),
                               blend_factor(desc->blend.dst ? desc->blend.dst : 1),
                               blend_factor(desc->blend.src_alpha ? desc->blend.src_alpha : 2),
                               blend_factor(desc->blend.dst_alpha ? desc->blend.dst_alpha : 1));
        dkColorStateSetBlendEnable(&p->color, t, desc->blend.enable && t < p->nrt);
        dkColorWriteStateSetMask(&p->cw, t, t < p->nrt ? desc->blend.write_mask : 0);
    }

    dkDepthStencilStateDefaults(&p->ds);
    p->ds.depthTestEnable = desc->depth_test ? 1 : 0;
    p->ds.depthWriteEnable = desc->depth_write ? 1 : 0;
    p->ds.depthCompareOp = (DkCompareOp)(desc->depth_func ? desc->depth_func : GPU_CMP_LESS);
    p->depth_write = desc->depth_write;
    if (desc->stencil_test) {
        p->ds.stencilTestEnable = 1;
        p->ds.stencilFrontFailOp = (DkStencilOp)desc->stencil_front.fail;
        p->ds.stencilFrontPassOp = (DkStencilOp)desc->stencil_front.pass;
        p->ds.stencilFrontDepthFailOp = (DkStencilOp)desc->stencil_front.depth_fail;
        p->ds.stencilFrontCompareOp = (DkCompareOp)desc->stencil_front.func;
        p->ds.stencilBackFailOp = (DkStencilOp)desc->stencil_back.fail;
        p->ds.stencilBackPassOp = (DkStencilOp)desc->stencil_back.pass;
        p->ds.stencilBackDepthFailOp = (DkStencilOp)desc->stencil_back.depth_fail;
        p->ds.stencilBackCompareOp = (DkCompareOp)desc->stencil_back.func;
        p->stencil_read = desc->stencil_read_mask;
        p->stencil_write = desc->stencil_write_mask;
    }

    p->nattr = desc->vertex.count > GPU_MAX_VERTEX_ATTRS ? GPU_MAX_VERTEX_ATTRS : desc->vertex.count;
    p->stride = desc->vertex.stride;
    for (uint32_t a = 0; a < p->nattr; a++) {
        DkVtxAttribState* v = &p->attr[a];
        memset(v, 0, sizeof *v);
        v->bufferId = 0;
        v->offset = desc->vertex.attr[a].offset;
        if (!vtx_format(desc->vertex.attr[a].format, v)) {
            mutexUnlock(&dd->lock);
            printf("[dk] pipeline_create: vertex format %u is not mapped\n", desc->vertex.attr[a].format);
            return none;
        }
    }
    p->strip_cut = desc->strip_cut;
    p->live = 1;
    gpu_pipeline h = {++dd->npipe};
    mutexUnlock(&dd->lock);
    if (how) *how = GPU_PIPELINE_COMPILED;
    return h;
}

static int dk_pipeline_cache_load(gpu_device* d, const char* path) { (void)d; (void)path; return GPU_PIPELINE_CACHE_NONE; }
static int dk_pipeline_cache_save(gpu_device* d, const char* path) { (void)d; (void)path; return 0; }

static gpu_query_pool dk_query_pool_create(gpu_device* d, uint32_t type, uint32_t count)
{
    DkDev* dd = DD(d);
    gpu_query_pool none = {0};
    if (dd->nqpool >= MAX_QPOOLS || !count) return none;
    QPool* q = &dd->qpool[dd->nqpool];
    q->a = mem_alloc(dd, MEM_GPU, (uint64_t)count * 16, 256);
    if (!q->a.chunk) return none;
    q->gpu = dd->chunks[q->a.chunk - 1].gpu + q->a.off;
    q->type = type;
    q->count = count;
    q->live = 1;
    gpu_query_pool h = {++dd->nqpool};
    return h;
}

/* ---------------------------------------------------------------------------
 * Recorders and submits
 * -------------------------------------------------------------------------*/

static void add_mem(void* user, DkCmdBuf cb, size_t min_size)
{
    gpu_cmd* c = (gpu_cmd*)user;
    DkDev* dd = DD(c->head.dev);
    if (c->used_mem < c->nmem) {   /* one it had before: begun again, it is free */
        dkCmdBufAddMemory(cb, c->mem[c->used_mem], 0, dkMemBlockGetSize(c->mem[c->used_mem]));
        c->used_mem++;
        return;
    }
    if (c->nmem == sizeof c->mem / sizeof c->mem[0]) {
        printf("[dk] a recorder ran out of command memory\n");
        return;
    }
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, dd->dev, (uint32_t)align_up(min_size > CMD_MEM ? min_size : CMD_MEM, DK_MEMBLOCK_ALIGNMENT));
    DkMemBlock b = dkMemBlockCreate(&m);
    if (!b) return;
    c->mem[c->nmem++] = b;
    c->used_mem = c->nmem;
    dkCmdBufAddMemory(cb, b, 0, m.size);
}

static void bind_descriptors(gpu_cmd* c)
{
    DkDev* dd = DD(c->head.dev);
    dkCmdBufBindImageDescriptorSet(c->cb, dd->img_desc_gpu, IMAGE_DESCS);
    dkCmdBufBindSamplerDescriptorSet(c->cb, dd->smp_desc_gpu, SAMPLER_DESCS);
    c->desc_epoch = dd->desc_epoch;
}

static void trace(gpu_cmd* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

static gpu_cmd* dk_cmd_begin(gpu_device* d, uint32_t slot)
{
    DkDev* dd = DD(d);
    if (slot >= dd->ncmd) return NULL;
    gpu_cmd* c = &dd->cmd[slot];
    run_frees(dd);
    dkCmdBufClear(c->cb);
    c->used_mem = 0;
    add_mem(c, c->cb, CMD_MEM);
    c->open = 1;
    c->layout = c->pipeline = 0;
    memset(c->table, 0, sizeof c->table);
    c->tex_dirty = 0;
    c->prim = DkPrimitive_Triangles;
    c->nrt = c->has_ds = 0;
    memset(&c->pass, 0, sizeof c->pass);
    c->scissor.x = c->scissor.y = 0;
    c->scissor.width = c->scissor.height = 0;
    c->stencil_ref = 0;
    c->idx_size = 2;
    /* what the CPU wrote since the last frame (descriptors, uploads) reaches the GPU's caches */
    dkCmdBufBarrier(c->cb, DkBarrier_None,
                    DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors |
                        DkInvalidateFlags_L2Cache);
    bind_descriptors(c);
    c->barrier = 0;
    trace(c, "cmd_begin: slot %u", slot);
    return c;
}

static void dk_cmd_end(gpu_device* d, gpu_cmd* c)
{
    (void)d;
    if (!c || !c->open) return;
    dkCmdBufBarrier(c->cb, DkBarrier_Full, DkInvalidateFlags_L2Cache);   /* (readbacks: the GPU's writes reach memory) */
    c->list = dkCmdBufFinishList(c->cb);
    c->open = 0;
}

static gpu_fence dk_submit(gpu_device* d, gpu_cmd* const* cmds, uint32_t n, const gpu_submit_desc* how)
{
    DkDev* dd = DD(d);
    for (uint32_t i = 0; i < n; i++)
        if (cmds[i] && cmds[i]->list) {
            dkQueueSubmitCommands(dd->queue, cmds[i]->list);
            cmds[i]->list = 0;
        }
    dd->submits++;
    if (how && (how->flags & GPU_SUBMIT_SIGNAL)) return dk_signal(d);
    dkQueueFlush(dd->queue);
    return 0;
}

static int dk_cmd_split(gpu_device* d, gpu_cmd* c, const gpu_submit_desc* how)
{
    (void)d; (void)c; (void)how;
    return 1;   /* (not split: it goes on as one) */
}

static uint64_t dk_gpu_ns(gpu_device* d, uint64_t* n)
{
    (void)d;
    if (n) *n = 0;
    return 0;
}

/* ---------------------------------------------------------------------------
 * The swap chain
 * -------------------------------------------------------------------------*/

static gpu_swapchain dk_swap_create(gpu_device* d, const gpu_swapchain_desc* desc)
{
    DkDev* dd = DD(d);
    gpu_swapchain none = {0};
    Swap* s = NULL;
    uint32_t si = 0;
    for (; si < 2; si++)
        if (!dd->swap[si].live) { s = &dd->swap[si]; break; }
    if (!s) return none;
    memset(s, 0, sizeof *s);
    s->n = desc->buffers < 2 ? 2 : desc->buffers > MAX_IMAGES ? MAX_IMAGES : desc->buffers;
    s->width = desc->width ? desc->width : 1280;
    s->height = desc->height ? desc->height : 720;
    s->format = desc->format == GPU_FORMAT_B8G8R8A8_UNORM ? desc->format : GPU_FORMAT_R8G8B8A8_UNORM;
    const Fmt f = fmt_of(s->format, GPU_USE_RT);
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, dd->dev);
    m.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression |
              DkImageFlags_Usage2DEngine;
    m.format = f.dk;
    m.dimensions[0] = s->width;
    m.dimensions[1] = s->height;
    DkImageLayout l;
    dkImageLayoutInitialize(&l, &m);
    const uint32_t align = dkImageLayoutGetAlignment(&l);
    const uint32_t size = (uint32_t)align_up(dkImageLayoutGetSize(&l), align);
    DkMemBlockMaker bm;
    dkMemBlockMakerDefaults(&bm, dd->dev, (uint32_t)align_up((uint64_t)size * s->n, DK_MEMBLOCK_ALIGNMENT));
    bm.flags = kind_flags(MEM_IMAGE);
    s->mem = dkMemBlockCreate(&bm);
    if (!s->mem) return none;
    DkImage const* list[MAX_IMAGES];
    for (uint32_t i = 0; i < s->n; i++) {
        const uint32_t ti = take(&dd->ntex, MAX_TEXTURES, dd->tex_free, &dd->ntex_free);
        if (ti == UINT32_MAX) return none;
        Tex* t = &dd->tex[ti];
        memset(t, 0, sizeof *t);
        dkImageInitialize(&t->image, &l, s->mem, i * size);
        t->live = t->swap = 1;
        t->fmt = f.dk;
        t->format = s->format;
        t->width = s->width;
        t->height = s->height;
        t->layers = t->mips = t->samples = 1;
        t->use = GPU_USE_RT | GPU_USE_COPY_DST | GPU_USE_COPY_SRC;
        t->bpb = 4;
        s->image[i].id = ti + 1;
        list[i] = &t->image;
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, dd->dev, nwindowGetDefault(), list, s->n);
    s->sc = dkSwapchainCreate(&sm);
    if (!s->sc) return none;
    s->live = 1;
    s->current = -1;
    printf("[dk] swap chain: %u images of %ux%u on the default window\n", s->n, s->width, s->height);
    gpu_swapchain h = {si + 1};
    return h;
}

static Swap* swap_of(DkDev* dd, gpu_swapchain sc)
{
    return sc.id && sc.id <= 2 && dd->swap[sc.id - 1].live ? &dd->swap[sc.id - 1] : NULL;
}

static void dk_swap_destroy(gpu_device* d, gpu_swapchain sc)
{
    DkDev* dd = DD(d);
    Swap* s = swap_of(dd, sc);
    if (!s) return;
    dkQueueWaitIdle(dd->queue);
    dkSwapchainDestroy(s->sc);
    for (uint32_t i = 0; i < s->n; i++) {
        Tex* t = &dd->tex[s->image[i].id - 1];
        t->swap = 0;
        defer_free(dd, FREE_TEX, s->image[i].id, 0);
    }
    dkMemBlockDestroy(s->mem);
    memset(s, 0, sizeof *s);
}

static int dk_swap_resize(gpu_device* d, gpu_swapchain sc, uint32_t width, uint32_t height, gpu_format format)
{
    (void)d; (void)sc; (void)width; (void)height; (void)format;
    return 0;   /* (the window has one size; the system scales it when docked) */
}

static uint32_t dk_swap_acquire(gpu_device* d, gpu_swapchain sc, gpu_texture* image)
{
    DkDev* dd = DD(d);
    Swap* s = swap_of(dd, sc);
    if (!s) return 0;
    if (s->current < 0) s->current = dkQueueAcquireImage(dd->queue, s->sc);
    if (image) *image = s->image[s->current];
    return (uint32_t)s->current;
}

static uint32_t dk_swap_image_count(gpu_device* d, gpu_swapchain sc)
{
    Swap* s = swap_of(DD(d), sc);
    return s ? s->n : 0;
}

static gpu_texture dk_swap_image(gpu_device* d, gpu_swapchain sc, uint32_t index)
{
    gpu_texture none = {0};
    Swap* s = swap_of(DD(d), sc);
    return s && index < s->n ? s->image[index] : none;
}

static int dk_swap_present(gpu_device* d, gpu_swapchain sc, uint32_t interval, uint32_t flags)
{
    DkDev* dd = DD(d);
    (void)flags;
    Swap* s = swap_of(dd, sc);
    if (!s || s->current < 0) return GPU_PRESENT_FAILED;
    if (dk_lost(d)) return GPU_PRESENT_LOST;
    dkSwapchainSetSwapInterval(s->sc, interval);
    dkQueuePresentImage(dd->queue, s->sc, s->current);
    s->current = -1;
    s->presents++;
    return GPU_PRESENTED;
}

static int dk_swap_stats(gpu_device* d, gpu_swapchain sc, gpu_present_stats* out)
{
    (void)d; (void)sc; (void)out;
    return 1;
}

static int dk_swap_hdr(gpu_device* d, gpu_swapchain sc, uint32_t colour_space, const gpu_hdr_metadata* meta)
{
    (void)d; (void)sc; (void)meta;
    return colour_space == GPU_COLOR_SRGB;
}

/* Only on a lost device (gpu.h): the core calls it after every frame's submit. */
static void dk_fault_report(gpu_device* d, const char* where)
{
    if (!dk_lost(d)) return;
    static int said;
    if (said++) return;
    printf("[dk] the device is lost (%s): deko3D says no more than that its queue is in its error state\n",
           where ? where : "?");
}

static void* dk_native(gpu_device* d, uint32_t kind, uint64_t handle)
{
    DkDev* dd = DD(d);
    (void)handle;
    if (kind == GPU_NATIVE_DEVICE) return dd->dev;
    if (kind == GPU_NATIVE_QUEUE) return dd->queue;
    return NULL;
}

void gpu_dk_report(gpu_device* d, uint32_t presents)
{
    DkDev* dd = DD(d);
    printf("[dk] present %u: %llu MB of GPU memory in %u blocks, %u textures, %u buffers, %u pipelines, %u shaders, "
           "fence %llu of %llu\n",
           presents, (unsigned long long)(dd->mem_used >> 20), dd->nchunks, dd->ntex - dd->ntex_free,
           dd->nbuf - dd->nbuf_free, dd->npipe, dd->nshd - dd->nshd_free, (unsigned long long)dd->completed,
           (unsigned long long)dd->signalled);
}

/* ---------------------------------------------------------------------------
 * Commands
 * -------------------------------------------------------------------------*/

static void flush_barrier(gpu_cmd* c)
{
    if (c->barrier) {
        dkCmdBufBarrier(c->cb, DkBarrier_Full, c->barrier & ~0x80000000u);
        c->barrier = 0;
    }
    DkDev* dd = DD(c->head.dev);
    if (c->desc_epoch != dd->desc_epoch) {
        dkCmdBufBarrier(c->cb, DkBarrier_None, DkInvalidateFlags_Descriptors);
        c->desc_epoch = dd->desc_epoch;
    }
}

/* DESNX_DK_TRACE=<n> (settings.txt): each of the first n commands that give the GPU work is sent
 * on its own and waited for, and the first one after which the queue is in its error state is
 * named in the log with its arguments. For finding what the GPU faults on; slow. */
static int s_trace = -1;
static uint32_t s_traced;
static void trace(gpu_cmd* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void trace(gpu_cmd* c, const char* fmt, ...)
{
    if (s_trace < 0) {
        const char* e = getenv("DESNX_DK_TRACE");
        s_trace = e ? atoi(e) : 0;
        if (s_trace > 0) printf("[dk] trace: the first %d commands one by one (DESNX_DK_TRACE)\n", s_trace);
    }
    if (!s_trace || s_traced >= (uint32_t)s_trace) return;
    s_traced++;
    DkDev* dd = DD(c->head.dev);
    dkQueueSubmitCommands(dd->queue, dkCmdBufFinishList(c->cb));
    dkQueueWaitIdle(dd->queue);
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (dkQueueIsInErrorState(dd->queue)) {
        printf("[dk] trace: command %u left the queue in its error state: %s\n", s_traced, line);
        s_trace = 0;
    } else if (s_traced <= 64 || s_traced % 256 == 0) {
        printf("[dk] trace: command %u done: %s\n", s_traced, line);
    }
    if (s_trace && s_traced == (uint32_t)s_trace) printf("[dk] trace: %u commands, none faulted\n", s_traced);
}

/* A barrier before what may read what the last targets were given: a pass on other targets, a
 * copy or a resolve. D3D12 lets the core leave some out (a texture rendered to, then read, with
 * implicit state changes); on deko3D a read before the GPU has written its tiles out shows the
 * old contents in squares and stripes. DESNX_DK_PASS_BARRIER=0 leaves it to the core's own. */
static void pass_barrier(gpu_cmd* c)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("DESNX_DK_PASS_BARRIER");
        on = !(e && *e == '0');
    }
    if (on && c->drawn) c->barrier |= 0x80000000u | DkInvalidateFlags_Image;
    c->drawn = 0;
}

static void bind_pass(gpu_cmd* c)
{
    DkImageView const* colours[4];
    for (uint32_t i = 0; i < c->nrt; i++) colours[i] = &c->rt[i];
    dkCmdBufBindRenderTargets(c->cb, colours, c->nrt, c->has_ds ? &c->ds : NULL);
}

void gpu_dk_cmd_pass_begin(gpu_cmd* c, const gpu_pass* pass)
{
    DkDev* dd = DD(c->head.dev);
    if (memcmp(&c->pass, pass, sizeof *pass)) pass_barrier(c);
    c->pass = *pass;
    c->nrt = 0;
    for (uint32_t i = 0; i < pass->nrt && i < 4; i++) {
        const View* v = view_of(dd, pass->rt[i]);
        if (v) c->rt[c->nrt++] = v->view;
    }
    const View* ds = view_of(dd, pass->ds);
    c->has_ds = ds != NULL;
    if (ds) c->ds = ds->view;
    flush_barrier(c);
    bind_pass(c);
    trace(c, "pass_begin: %u targets%s", c->nrt, c->has_ds ? " and depth" : "");
}

void gpu_dk_cmd_pass_end(gpu_cmd* c) { (void)c; }

/* A view's size, at its mip. */
static void view_size(DkDev* dd, const View* v, uint32_t* w, uint32_t* h)
{
    const Tex* t = &dd->tex[v->tex];
    *w = t->width >> v->desc.mip;
    *h = t->height >> v->desc.mip;
    if (!*w) *w = 1;
    if (!*h) *h = 1;
}

static void clear_scissor(gpu_cmd* c, DkDev* dd, const View* v, const gpu_rect* rect)
{
    uint32_t w, h;
    view_size(dd, v, &w, &h);
    DkScissor s = {0, 0, w, h};
    if (rect) {
        s.x = rect->left > 0 ? (uint32_t)rect->left : 0;
        s.y = rect->top > 0 ? (uint32_t)rect->top : 0;
        s.width = rect->right > rect->left ? (uint32_t)(rect->right - rect->left) : 0;
        s.height = rect->bottom > rect->top ? (uint32_t)(rect->bottom - rect->top) : 0;
    }
    dkCmdBufSetScissors(c->cb, 0, &s, 1);
}

static void restore_scissor(gpu_cmd* c)
{
    if (c->scissor.width) dkCmdBufSetScissors(c->cb, 0, &c->scissor, 1);
}

/* DESNX_DK_TINT=1 (settings.txt): a clear to black clears to a colour that goes round the hues
 * every few seconds instead, so that the screen shows the clears, copies, blit and presents at
 * work while the game's own draws have no shaders (stage 12b's test). */
static const float* tint(DkDev* dd, const float rgba[4], float out[4])
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("DESNX_DK_TINT");
        on = e && *e && *e != '0';
    }
    if (!on || rgba[0] != 0.0f || rgba[1] != 0.0f || rgba[2] != 0.0f) return rgba;
    const float t = (float)dd->submits / 90.0f;
    out[0] = 0.5f + 0.5f * sinf(t);
    out[1] = 0.5f + 0.5f * sinf(t + 2.094f);
    out[2] = 0.5f + 0.5f * sinf(t + 4.189f);
    out[3] = 1.0f;
    return out;
}

void gpu_dk_cmd_clear_color(gpu_cmd* c, gpu_view rtv, const float rgba_in[4], const gpu_rect* rect)
{
    DkDev* dd = DD(c->head.dev);
    const View* v = view_of(dd, rtv);
    if (!v) return;
    float tinted[4];
    const float* rgba = tint(dd, rgba_in, tinted);
    flush_barrier(c);
    int at = -1;
    for (uint32_t i = 0; i < c->pass.nrt && i < 4; i++)
        if (c->pass.rt[i].id == rtv.id) { at = (int)i; break; }
    if (at < 0) {   /* a view not bound: bound alone for the clear, then the pass again */
        DkImageView const* one = &v->view;
        dkCmdBufBindRenderTargets(c->cb, &one, 1, NULL);
    }
    clear_scissor(c, dd, v, rect);
    dkCmdBufClearColorFloat(c->cb, at < 0 ? 0 : (uint32_t)at, DkColorMask_RGBA, rgba[0], rgba[1], rgba[2], rgba[3]);
    c->drawn = 1;
    restore_scissor(c);
    if (at < 0) bind_pass(c);
    trace(c, "clear_color: view %u (texture %u, format %u, %ux%u)%s", rtv.id, v->tex + 1, dd->tex[v->tex].format,
          dd->tex[v->tex].width, dd->tex[v->tex].height, at < 0 ? ", not bound" : "");
}

void gpu_dk_cmd_clear_depth(gpu_cmd* c, gpu_view dsv, uint32_t flags, float depth, uint8_t stencil, const gpu_rect* rect)
{
    DkDev* dd = DD(c->head.dev);
    const View* v = view_of(dd, dsv);
    if (!v) return;
    flush_barrier(c);
    const int bound = c->pass.ds.id == dsv.id;
    if (!bound) dkCmdBufBindRenderTargets(c->cb, NULL, 0, &v->view);
    clear_scissor(c, dd, v, rect);
    dkCmdBufClearDepthStencil(c->cb, (flags & GPU_CLEAR_DEPTH) != 0, depth, (flags & GPU_CLEAR_STENCIL) ? 0xFF : 0, stencil);
    c->drawn = 1;
    restore_scissor(c);
    if (!bound) bind_pass(c);
    trace(c, "clear_depth: view %u (format %u)%s", dsv.id, dd->tex[v->tex].format, bound ? "" : ", not bound");
}

void gpu_dk_cmd_set_layout(gpu_cmd* c, gpu_layout layout)
{
    c->layout = layout.id;
    memset(c->table, 0, sizeof c->table);
    c->tex_dirty = 1;
    DkDev* dd = DD(c->head.dev);
    if (!layout.id || layout.id > dd->nlay) return;
    const Lay* l = &dd->lay[layout.id - 1];
    /* each CONST32 slot's uniform buffer, at its register */
    for (uint32_t s = 0; s < l->desc.nslots; s++)
        if (l->desc.slot[s].kind == GPU_SLOT_CONST32)
            for (uint32_t st = 0; st < DkStage_MaxGraphics; st++)
                if (l->stages[s] & (1u << st))
                    dkCmdBufBindUniformBuffer(c->cb, (DkStage)st, l->desc.slot[s].reg & 15,
                                              c->push_gpu + s * PUSH_UBO, PUSH_UBO);
}

void gpu_dk_cmd_set_pipeline(gpu_cmd* c, gpu_pipeline pipeline)
{
    DkDev* dd = DD(c->head.dev);
    if (!pipeline.v || pipeline.v > dd->npipe) return;
    const Pipe* p = &dd->pipe[pipeline.v - 1];
    c->pipeline = (uint32_t)pipeline.v;
    dkCmdBufBindShaders(c->cb, p->stage_mask, p->shaders, p->nshaders);
    dkCmdBufBindRasterizerState(c->cb, &p->rast);
    dkCmdBufBindMultisampleState(c->cb, &p->ms);
    dkCmdBufBindColorState(c->cb, &p->color);
    dkCmdBufBindColorWriteState(c->cb, &p->cw);
    dkCmdBufBindBlendStates(c->cb, 0, p->blend, 4);
    dkCmdBufBindDepthStencilState(c->cb, &p->ds);
    if (p->ds.stencilTestEnable) {
        dkCmdBufSetStencil(c->cb, DkFace_FrontAndBack, p->stencil_write, (uint8_t)c->stencil_ref, p->stencil_read);
    }
    if (p->nattr) {
        dkCmdBufBindVtxAttribState(c->cb, p->attr, p->nattr);
        const DkVtxBufferState vb = {p->stride, 0};
        dkCmdBufBindVtxBufferState(c->cb, &vb, 1);
    } else {
        dkCmdBufBindVtxAttribState(c->cb, NULL, 0);
        dkCmdBufBindVtxBufferState(c->cb, NULL, 0);
    }
    dkCmdBufSetPrimitiveRestart(c->cb, p->strip_cut != GPU_STRIP_CUT_NONE,
                                p->strip_cut == GPU_STRIP_CUT_FFFF ? 0xFFFFu : 0xFFFFFFFFu);
    trace(c, "set_pipeline: %u (%u shaders, %u attributes)", (unsigned)pipeline.v, p->nshaders, p->nattr);
}

void gpu_dk_cmd_set_topology(gpu_cmd* c, uint32_t topology)
{
    switch (topology) {
    case GPU_TOPO_POINTLIST: c->prim = DkPrimitive_Points; break;
    case GPU_TOPO_LINELIST: c->prim = DkPrimitive_Lines; break;
    case GPU_TOPO_LINESTRIP: c->prim = DkPrimitive_LineStrip; break;
    case GPU_TOPO_TRIANGLESTRIP: c->prim = DkPrimitive_TriangleStrip; break;
    default: c->prim = DkPrimitive_Triangles; break;
    }
}

void gpu_dk_cmd_set_depth_write(gpu_cmd* c, uint32_t on)
{
    DkDev* dd = DD(c->head.dev);
    if (!c->pipeline) return;
    DkDepthStencilState ds = dd->pipe[c->pipeline - 1].ds;
    ds.depthWriteEnable = on && dd->pipe[c->pipeline - 1].depth_write ? 1 : 0;
    dkCmdBufBindDepthStencilState(c->cb, &ds);
}

void gpu_dk_cmd_set_viewport(gpu_cmd* c, const gpu_viewport* vp)
{
    const DkViewport v = {vp->x, vp->y, vp->width, vp->height, vp->min_depth, vp->max_depth};
    dkCmdBufSetViewports(c->cb, 0, &v, 1);
}

void gpu_dk_cmd_set_scissor(gpu_cmd* c, const gpu_rect* rect)
{
    c->scissor.x = rect->left > 0 ? (uint32_t)rect->left : 0;
    c->scissor.y = rect->top > 0 ? (uint32_t)rect->top : 0;
    c->scissor.width = rect->right > rect->left ? (uint32_t)(rect->right - rect->left) : 0;
    c->scissor.height = rect->bottom > rect->top ? (uint32_t)(rect->bottom - rect->top) : 0;
    dkCmdBufSetScissors(c->cb, 0, &c->scissor, 1);
}

void gpu_dk_cmd_set_table_pools(gpu_cmd* c, gpu_table_pool tex, gpu_table_pool samplers)
{
    (void)c; (void)tex; (void)samplers;   /* (every pool is in the device's one descriptor array each) */
}

static const Lay* lay_of(gpu_cmd* c)
{
    DkDev* dd = DD(c->head.dev);
    return c->layout && c->layout <= dd->nlay ? &dd->lay[c->layout - 1] : NULL;
}

void gpu_dk_cmd_bind_cb(gpu_cmd* c, uint32_t slot, gpu_addr addr)
{
    const Lay* l = lay_of(c);
    if (!l || slot >= l->desc.nslots || !addr) return;
    const uint32_t size = bytes_from(DD(c->head.dev), addr, DK_UNIFORM_BUF_MAX_SIZE) & ~0xFFu;
    for (uint32_t st = 0; st < DkStage_MaxGraphics; st++)
        if (l->stages[slot] & (1u << st))
            dkCmdBufBindUniformBuffer(c->cb, (DkStage)st, l->desc.slot[slot].reg & 15, addr, size ? size : 256);
    trace(c, "bind_cb: slot %u b%u at %#llx, %u bytes", slot, l->desc.slot[slot].reg, (unsigned long long)addr, size);
}

void gpu_dk_cmd_bind_raw(gpu_cmd* c, uint32_t slot, gpu_addr addr)
{
    const Lay* l = lay_of(c);
    if (!l || slot >= l->desc.nslots || !addr) return;
    const uint32_t reg = l->desc.slot[slot].reg;
    const uint32_t size = bytes_from(DD(c->head.dev), addr, 64u << 20);
    for (uint32_t st = 0; st < DkStage_MaxGraphics; st++)
        if (l->stages[slot] & (1u << st))
            dkCmdBufBindStorageBuffer(c->cb, (DkStage)st, (reg >= 16 ? reg - 16 : reg) & 15, addr, size);
    trace(c, "bind_raw: slot %u t%u at %#llx, %u bytes", slot, reg, (unsigned long long)addr, size);
}

void gpu_dk_cmd_bind_table(gpu_cmd* c, uint32_t slot, gpu_table table)
{
    if (slot < GPU_MAX_SLOTS) {
        c->table[slot] = table;
        c->tex_dirty = 1;
    }
}

void gpu_dk_cmd_push_const(gpu_cmd* c, uint32_t slot, uint32_t count, const void* values, uint32_t first)
{
    if (slot >= GPU_MAX_SLOTS || !count || (first + count) * 4 > PUSH_UBO) return;
    dkCmdBufPushConstants(c->cb, c->push_gpu + slot * PUSH_UBO, PUSH_UBO, first * 4, count * 4, values);
    trace(c, "push_const: slot %u, %u values from %u", slot, count, first);
}

void gpu_dk_cmd_push_const1(gpu_cmd* c, uint32_t slot, uint32_t value, uint32_t first)
{
    gpu_dk_cmd_push_const(c, slot, 1, &value, first);
}

void gpu_dk_cmd_set_vb(gpu_cmd* c, uint32_t slot, gpu_addr addr, uint32_t size, uint32_t stride)
{
    if (slot >= DK_MAX_VERTEX_BUFFERS) return;
    dkCmdBufBindVtxBuffer(c->cb, slot, addr, size);
    const DkVtxBufferState vb = {stride, 0};
    if (slot == 0) dkCmdBufBindVtxBufferState(c->cb, &vb, 1);
}

void gpu_dk_cmd_set_ib(gpu_cmd* c, gpu_addr addr, uint32_t size, gpu_format format)
{
    (void)size;
    c->idx_size = format == GPU_FORMAT_R32_UINT ? 4 : 2;
    dkCmdBufBindIdxBuffer(c->cb, format == GPU_FORMAT_R32_UINT ? DkIdxFormat_Uint32 : DkIdxFormat_Uint16, addr);
}

/* The textures the layout's tables give, each with its sampler (docs/deko3d.md). */
static void bind_textures(gpu_cmd* c)
{
    const Lay* l = lay_of(c);
    c->tex_dirty = 0;
    if (!l) return;
    for (uint32_t s = 0; s < l->desc.nslots; s++) {
        const gpu_layout_slot* ts = &l->desc.slot[s];
        if (ts->kind != GPU_SLOT_TABLE_TEX || !c->table[s].v) continue;
        const uint32_t img0 = (uint32_t)(c->table[s].v & 0xFFFFFFFFu) - 1;
        DkResHandle h[DK_NUM_TEXTURE_BINDINGS];
        const uint32_t n = ts->count > DK_NUM_TEXTURE_BINDINGS ? DK_NUM_TEXTURE_BINDINGS : ts->count;
        for (uint32_t j = 0; j < n; j++) {
            const uint32_t reg = ts->reg + j;
            uint32_t smp = 0;
            int found = 0;
            for (uint32_t q = 0; q < l->desc.nslots && !found; q++) {
                const gpu_layout_slot* ss = &l->desc.slot[q];
                if (ss->kind == GPU_SLOT_TABLE_SAMPLER && c->table[q].v && reg >= ss->reg && reg < ss->reg + ss->count) {
                    smp = (uint32_t)(c->table[q].v & 0xFFFFFFFFu) - 1 + (reg - ss->reg);
                    found = 1;
                }
            }
            for (uint32_t q = 0; q < l->desc.nstatic && !found; q++)
                if (l->desc.statics[q].reg == reg) { smp = l->static_desc[q]; found = 1; }
            if (!found && l->desc.nstatic == 1) smp = l->static_desc[0];
            h[j] = dkMakeTextureHandle(img0 + j, smp);
        }
        for (uint32_t st = 0; st < DkStage_MaxGraphics; st++)
            if (l->stages[s] & (1u << st))
                dkCmdBufBindTextures(c->cb, (DkStage)st, ts->reg, h, n);
    }
}

static void before_draw(gpu_cmd* c)
{
    flush_barrier(c);
    if (c->tex_dirty) bind_textures(c);
}

void gpu_dk_cmd_draw(gpu_cmd* c, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance)
{
    if (!c->pipeline) return;
    before_draw(c);
    dkCmdBufDraw(c->cb, c->prim, vertices, instances ? instances : 1, first_vertex, first_instance);
    c->drawn = 1;
    trace(c, "draw: %u vertices from %u, primitive %u, pipeline %u", vertices, first_vertex, (unsigned)c->prim, c->pipeline);
}

void gpu_dk_cmd_draw_indexed(gpu_cmd* c, uint32_t indices, uint32_t instances, uint32_t first_index, int32_t base_vertex,
                             uint32_t first_instance)
{
    if (!c->pipeline) return;
    before_draw(c);
    dkCmdBufDrawIndexed(c->cb, c->prim, indices, instances ? instances : 1, first_index, base_vertex, first_instance);
    c->drawn = 1;
    trace(c, "draw_indexed: %u indices from %u, base %d, pipeline %u", indices, first_index, base_vertex, c->pipeline);
}

/* Barriers: gathered, and made coarse (deko3D has no per-resource ones). */
static uint32_t state_flags(gpu_state after)
{
    uint32_t f = 0x80000000u;   /* (a barrier, with no cache to invalidate) */
    if (after & (GPU_STATE_PIXEL_SHADER_RESOURCE | GPU_STATE_NON_PIXEL_SHADER_RESOURCE | GPU_STATE_COPY_SOURCE))
        f |= DkInvalidateFlags_Image;
    if (after & (GPU_STATE_VERTEX_AND_CONSTANT_BUFFER | GPU_STATE_INDEX_BUFFER)) f |= DkInvalidateFlags_Shader;
    if (after == GPU_STATE_COMMON) f |= DkInvalidateFlags_Image | DkInvalidateFlags_Shader;
    if (after & (GPU_STATE_DEPTH_WRITE | GPU_STATE_DEPTH_READ)) f |= DkInvalidateFlags_Zcull;
    return f;
}

void gpu_dk_cmd_barrier(gpu_cmd* c, gpu_texture texture, gpu_state before, gpu_state after)
{
    (void)texture; (void)before;
    c->barrier |= state_flags(after);
}

void gpu_dk_cmd_barrier_n(gpu_cmd* c, uint32_t n, const gpu_barrier* list)
{
    for (uint32_t i = 0; i < n; i++)
        c->barrier |= list[i].type == GPU_BARRIER_ALIASING ? 0x80000000u : state_flags(list[i].after);
}

/* A subresource's view for a copy: its mip, the whole of its layers (a rect's z picks one). */
static void copy_view(const Tex* t, uint32_t sub, DkImageView* v, uint32_t* layer)
{
    const uint32_t mip = sub % t->mips;
    *layer = (sub / t->mips) % t->layers;
    dkImageViewDefaults(v, &t->image);
    v->mipLevelOffset = (uint8_t)mip;
    v->mipLevelCount = 1;
    if (t->depth && sub / (t->mips * t->layers) == 1) v->dsSource = DkDsSource_Stencil;
}

static void sub_size(const Tex* t, uint32_t sub, uint32_t* w, uint32_t* h)
{
    const uint32_t mip = sub % t->mips;
    *w = t->width >> mip;
    *h = t->height >> mip;
    if (!*w) *w = 1;
    if (!*h) *h = 1;
}

void gpu_dk_cmd_copy_texture(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                             gpu_texture src, uint32_t src_sub, const gpu_box* box)
{
    DkDev* dd = DD(c->head.dev);
    const Tex* d = tex_of(dd, dst);
    const Tex* s = tex_of(dd, src);
    if (!d || !s) return;
    flush_barrier(c);
    DkImageView sv, dv;
    uint32_t sl, dl, w, h;
    copy_view(s, src_sub, &sv, &sl);
    copy_view(d, dst_sub, &dv, &dl);
    sub_size(s, src_sub, &w, &h);
    DkImageRect sr = {0, 0, sl, w, h, 1};
    if (box) {
        sr.x = box->left;
        sr.y = box->top;
        sr.width = box->right - box->left;
        sr.height = box->bottom - box->top;
    }
    const DkImageRect dr = {x, y, dl + z, sr.width, sr.height, 1};
    pass_barrier(c);
    flush_barrier(c);
    dkCmdBufCopyImage(c->cb, &sv, &sr, &dv, &dr, 0);
    c->drawn = 1;
    trace(c, "copy_texture: %u (format %u) sub %u %ux%u at %u,%u to %u (format %u) sub %u at %u,%u,%u", src.id, s->format,
          src_sub, sr.width, sr.height, sr.x, sr.y, dst.id, d->format, dst_sub, x, y, dr.z);
}

void gpu_dk_cmd_copy_texture_whole(gpu_cmd* c, gpu_texture dst, gpu_texture src)
{
    DkDev* dd = DD(c->head.dev);
    const Tex* s = tex_of(dd, src);
    if (!s) return;
    for (uint32_t sub = 0; sub < s->mips * s->layers; sub++)
        gpu_dk_cmd_copy_texture(c, dst, sub, 0, 0, 0, src, sub, NULL);
}

void gpu_dk_cmd_copy_buf_to_tex(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                                gpu_buffer src, const gpu_footprint* from)
{
    DkDev* dd = DD(c->head.dev);
    const Tex* t = tex_of(dd, dst);
    const Buf* b = buf_of(dd, src);
    if (!t || !b || !from) return;
    flush_barrier(c);
    DkImageView v;
    uint32_t layer;
    copy_view(t, dst_sub, &v, &layer);
    uint32_t w = from->width, h = from->height;
    uint32_t sw, sh;
    sub_size(t, dst_sub, &sw, &sh);
    if (x + w > sw) w = sw > x ? sw - x : 0;   /* (a BC footprint's whole blocks past a small mip) */
    if (y + h > sh) h = sh > y ? sh - y : 0;
    if (!w || !h) return;
    const uint32_t rows = t->block ? (from->height + 3) / 4 : from->height;
    const DkCopyBuf cb = {b->gpu + from->offset, from->row_pitch, rows};
    const DkImageRect r = {x, y, layer + z, w, h, 1};
    pass_barrier(c);
    flush_barrier(c);
    dkCmdBufCopyBufferToImage(c->cb, &cb, &v, &r, 0);
    c->drawn = 1;
    trace(c, "copy_buf_to_tex: buffer %u (%llu bytes) at %llu, pitch %u, %u rows, to %u (format %u, %ux%u) sub %u, %ux%u at %u,%u,%u",
          src.id, (unsigned long long)b->size, (unsigned long long)from->offset, from->row_pitch, rows, dst.id, t->format,
          t->width, t->height, dst_sub, w, h, x, y, r.z);
}

void gpu_dk_cmd_copy_tex_to_buf(gpu_cmd* c, gpu_buffer dst, const gpu_footprint* to, gpu_texture src, uint32_t src_sub,
                                const gpu_box* box)
{
    DkDev* dd = DD(c->head.dev);
    const Tex* t = tex_of(dd, src);
    const Buf* b = buf_of(dd, dst);
    if (!t || !b || !to) return;
    flush_barrier(c);
    DkImageView v;
    uint32_t layer, w, h;
    copy_view(t, src_sub, &v, &layer);
    sub_size(t, src_sub, &w, &h);
    DkImageRect r = {0, 0, layer, w, h, 1};
    if (box) {
        r.x = box->left;
        r.y = box->top;
        r.width = box->right - box->left;
        r.height = box->bottom - box->top;
    }
    const uint32_t rows = t->block ? (r.height + 3) / 4 : r.height;
    const DkCopyBuf cb = {b->gpu + to->offset, to->row_pitch, rows};
    pass_barrier(c);
    flush_barrier(c);
    dkCmdBufCopyImageToBuffer(c->cb, &v, &r, &cb, 0);
    c->drawn = 1;
    trace(c, "copy_tex_to_buf: %u (format %u) sub %u, %ux%u at %u,%u, to buffer %u (%llu bytes) at %llu, pitch %u", src.id,
          t->format, src_sub, r.width, r.height, r.x, r.y, dst.id, (unsigned long long)b->size,
          (unsigned long long)to->offset, to->row_pitch);
}

void gpu_dk_cmd_copy_buffer(gpu_cmd* c, gpu_buffer dst, uint64_t dst_offset, gpu_buffer src, uint64_t src_offset,
                            uint64_t size)
{
    DkDev* dd = DD(c->head.dev);
    const Buf* d = buf_of(dd, dst);
    const Buf* s = buf_of(dd, src);
    if (!d || !s || !size) return;
    flush_barrier(c);
    dkCmdBufCopyBuffer(c->cb, s->gpu + src_offset, d->gpu + dst_offset, (uint32_t)size);
    trace(c, "copy_buffer: %llu bytes from %u at %llu to %u at %llu", (unsigned long long)size, src.id,
          (unsigned long long)src_offset, dst.id, (unsigned long long)dst_offset);
}

void gpu_dk_cmd_resolve(gpu_cmd* c, gpu_texture dst, uint32_t dst_sub, gpu_texture src, uint32_t src_sub, gpu_format format)
{
    DkDev* dd = DD(c->head.dev);
    const Tex* d = tex_of(dd, dst);
    const Tex* s = tex_of(dd, src);
    (void)format;
    if (!d || !s) return;
    flush_barrier(c);
    DkImageView sv, dv;
    uint32_t sl, dl;
    copy_view(s, src_sub, &sv, &sl);
    copy_view(d, dst_sub, &dv, &dl);
    pass_barrier(c);
    flush_barrier(c);
    dkCmdBufResolveImage(c->cb, &sv, &dv);
    c->drawn = 1;
    trace(c, "resolve: %u to %u", src.id, dst.id);
}

static const QPool* qpool_of(gpu_cmd* c, gpu_query_pool pool)
{
    DkDev* dd = DD(c->head.dev);
    return pool.id && pool.id <= dd->nqpool ? &dd->qpool[pool.id - 1] : NULL;
}

void gpu_dk_cmd_query_reset(gpu_cmd* c, gpu_query_pool pool, uint32_t first, uint32_t count)
{
    (void)c; (void)pool; (void)first; (void)count;   /* (query_begin resets the counter) */
}

void gpu_dk_cmd_query_begin(gpu_cmd* c, gpu_query_pool pool, uint32_t index)
{
    const QPool* q = qpool_of(c, pool);
    if (!q || index >= q->count) return;
    dkCmdBufResetCounter(c->cb, DkCounter_SamplesPassed);
}

void gpu_dk_cmd_query_end(gpu_cmd* c, gpu_query_pool pool, uint32_t index)
{
    const QPool* q = qpool_of(c, pool);
    if (!q || index >= q->count) return;
    dkCmdBufReportCounter(c->cb, DkCounter_SamplesPassed, q->gpu + (uint64_t)index * 16);
    trace(c, "query_end: %u", index);
}

void gpu_dk_cmd_query_resolve(gpu_cmd* c, gpu_query_pool pool, uint32_t first, uint32_t count, gpu_buffer dst,
                              uint64_t offset)
{
    DkDev* dd = DD(c->head.dev);
    const QPool* q = qpool_of(c, pool);
    const Buf* b = buf_of(dd, dst);
    if (!q || !b || first + count > q->count) return;
    dkCmdBufBarrier(c->cb, DkBarrier_Full, 0);
    for (uint32_t i = 0; i < count; i++)   /* each report is {value, timestamp}: the value */
        dkCmdBufCopyBuffer(c->cb, q->gpu + (uint64_t)(first + i) * 16, b->gpu + offset + (uint64_t)i * 8, 8);
    trace(c, "query_resolve: %u from %u", count, first);
}

void gpu_dk_cmd_set_stencil_ref(gpu_cmd* c, uint32_t ref)
{
    DkDev* dd = DD(c->head.dev);
    c->stencil_ref = ref;
    if (!c->pipeline) return;
    const Pipe* p = &dd->pipe[c->pipeline - 1];
    dkCmdBufSetStencil(c->cb, DkFace_FrontAndBack, p->stencil_write, (uint8_t)ref, p->stencil_read);
}

void gpu_dk_cmd_set_blend_factor(gpu_cmd* c, const float rgba[4])
{
    dkCmdBufSetBlendConst(c->cb, rgba[0], rgba[1], rgba[2], rgba[3]);
}

void gpu_dk_cmd_marker(gpu_cmd* c, const char* text, uint32_t len)
{
    (void)c; (void)text; (void)len;
}

/* The per-draw device calls. */
void gpu_dk_fast_table_write_tex(gpu_device* d, gpu_table_pool pool, uint32_t index, const gpu_view_desc* srv)
{
    dk_table_write_tex(d, pool, index, srv);
}
void gpu_dk_fast_table_writer_tex(gpu_device* d, gpu_table_writer w, uint32_t index, const gpu_view_desc* srv)
{
    dk_table_writer_tex(d, w, index, srv);
}
void gpu_dk_fast_table_write_sampler(gpu_device* d, gpu_table_pool pool, uint32_t index, gpu_sampler s)
{
    dk_table_write_sampler(d, pool, index, s);
}
gpu_table gpu_dk_fast_table_ref(gpu_device* d, gpu_table_pool pool, uint32_t index)
{
    return dk_table_ref(d, pool, index);
}
gpu_addr gpu_dk_fast_buffer_addr(gpu_device* d, gpu_buffer b) { return dk_buffer_addr(d, b); }

/* The tables of functions. */
#define DK_VT_ENTRY(name, form, ret, params, args) .name = dk_##name,
static const gpu_vt k_vt = { GPU_VT_FUNCS(DK_VT_ENTRY) };
#undef DK_VT_ENTRY

#define DK_OP_ENTRY(name, form, ret, params, args) .name = gpu_dk_cmd_##name,
static const gpu_cmd_ops k_cmd_ops = { GPU_CMD_OPS(DK_OP_ENTRY) };
#undef DK_OP_ENTRY

/* ---------------------------------------------------------------------------
 * The start
 * -------------------------------------------------------------------------*/

static void debug_cb(void* user, const char* context, DkResult result, const char* message)
{
    (void)user;
    printf("[dk] deko3D: %s: %s (result %d)\n", context ? context : "?", message ? message : "?", (int)result);
}

static DkMemBlock block(DkDev* dd, uint32_t size, uint32_t flags)
{
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, dd->dev, (uint32_t)align_up(size, DK_MEMBLOCK_ALIGNMENT));
    m.flags = flags;
    return dkMemBlockCreate(&m);
}

gpu_device* gpu_dk_create(const gpu_device_desc* desc)
{
    DkDev* dd = calloc(1, sizeof *dd);
    if (!dd) return NULL;
    dd->base.vt = &k_vt;
    dd->base.caps.api = GPU_API_VULKAN;   /* (the nearest: explicit memory, y as D3D12 has it here) */
    dd->base.caps.bits = GPU_CAP_GS | GPU_CAP_D24S8 | GPU_CAP_DEPTH_CLAMP | GPU_CAP_MIRROR_ONCE | GPU_CAP_BORDER |
                         GPU_CAP_ANISO16 | GPU_CAP_SWIZZLE | GPU_CAP_BC | GPU_CAP_RGBA32F_BLEND |
                         GPU_CAP_RGBA32F_FILTER | GPU_CAP_OCCLUSION_PRECISE | GPU_CAP_NULL_DESCRIPTOR |
                         GPU_CAP_UNUSED_ATTACHMENTS;
    if (desc) dd->base.caps.bits &= ~desc->cap_mask;
    dd->base.caps.cb_align = DK_UNIFORM_BUF_ALIGNMENT;
    dd->base.caps.max_aniso = 16;
    dd->base.caps.host_import_align = 0;

    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = debug_cb;
    dd->dev = dkDeviceCreate(&dm);
    if (!dd->dev) { free(dd); return NULL; }
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, dd->dev);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_HighPrio;
    qm.commandMemorySize = 4u << 20;
    qm.flushThreshold = 256u << 10;
    dd->queue = dkQueueCreate(&qm);

    dd->buf = calloc(MAX_BUFFERS, sizeof *dd->buf);
    dd->buf_free = calloc(MAX_BUFFERS, sizeof *dd->buf_free);
    dd->tex = calloc(MAX_TEXTURES, sizeof *dd->tex);
    dd->tex_free = calloc(MAX_TEXTURES, sizeof *dd->tex_free);
    dd->view = calloc(MAX_VIEWS, sizeof *dd->view);
    dd->view_free = calloc(MAX_VIEWS, sizeof *dd->view_free);
    dd->smp = calloc(MAX_SAMPLERS, sizeof *dd->smp);
    dd->shd = calloc(MAX_SHADERS, sizeof *dd->shd);
    dd->shd_free = calloc(MAX_SHADERS, sizeof *dd->shd_free);
    dd->pipe = calloc(MAX_PIPELINES, sizeof *dd->pipe);
    const uint32_t flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    dd->img_desc_mem = block(dd, IMAGE_DESCS * sizeof(DkImageDescriptor), flags);
    dd->smp_desc_mem = block(dd, SAMPLER_DESCS * sizeof(DkSamplerDescriptor), flags);
    dd->code_mem = block(dd, CODE_SIZE, flags | DkMemBlockFlags_Code);
    if (!dd->queue || !dd->buf || !dd->tex || !dd->view || !dd->smp || !dd->shd || !dd->pipe || !dd->img_desc_mem ||
        !dd->smp_desc_mem || !dd->code_mem || !dd->buf_free || !dd->tex_free || !dd->view_free || !dd->shd_free) {
        printf("[dk] the device's start failed: out of memory\n");
        return NULL;
    }
    dd->img_desc = dkMemBlockGetCpuAddr(dd->img_desc_mem);
    dd->smp_desc = dkMemBlockGetCpuAddr(dd->smp_desc_mem);
    dd->img_desc_gpu = dkMemBlockGetGpuAddr(dd->img_desc_mem);
    dd->smp_desc_gpu = dkMemBlockGetGpuAddr(dd->smp_desc_mem);
    dd->code_cpu = dkMemBlockGetCpuAddr(dd->code_mem);

    /* what an empty entry reads: a 1x1 texture of zeroes, R8 and RGBA8, and a point sampler */
    gpu_texture_desc td;
    memset(&td, 0, sizeof td);
    td.width = td.height = 1;
    td.layers = td.mips = 1;
    td.samples = 1;
    td.use = GPU_USE_SAMPLED;
    td.format = GPU_FORMAT_R8_UNORM;
    dd->dummy_r8 = dk_texture_create(&dd->base, &td);
    td.format = GPU_FORMAT_R8G8B8A8_UNORM;
    dd->dummy_rgba8 = dk_texture_create(&dd->base, &td);
    if (!dd->dummy_r8.id || !dd->dummy_rgba8.id) return NULL;
    DkImageView v;
    dkImageViewDefaults(&v, &dd->tex[dd->dummy_r8.id - 1].image);
    dkImageDescriptorInitialize(&dd->empty_r8, &v, false, false);
    dkImageViewDefaults(&v, &dd->tex[dd->dummy_rgba8.id - 1].image);
    dkImageDescriptorInitialize(&dd->empty_rgba8, &v, false, false);
    gpu_sampler_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.address_u = sd.address_v = sd.address_w = GPU_ADDRESS_CLAMP;
    sd.max_lod = GPU_LOD_MAX;
    sd.compare = GPU_CMP_NEVER;
    sampler_desc(dd, &sd, &dd->empty_smp);

    /* the recorders */
    dd->ncmd = desc && desc->cmd_slots ? (desc->cmd_slots > MAX_SLOTS ? MAX_SLOTS : desc->cmd_slots) : 2;
    for (uint32_t i = 0; i < dd->ncmd; i++) {
        gpu_cmd* c = &dd->cmd[i];
        c->head.ops = &k_cmd_ops;
        c->head.dev = &dd->base;
        c->slot = i;
        DkCmdBufMaker cm;
        dkCmdBufMakerDefaults(&cm, dd->dev);
        cm.userData = c;
        cm.cbAddMem = add_mem;
        c->cb = dkCmdBufCreate(&cm);
        c->push = block(dd, GPU_MAX_SLOTS * PUSH_UBO, flags);
        if (!c->cb || !c->push) return NULL;
        c->push_gpu = dkMemBlockGetGpuAddr(c->push);
    }
    printf("[dk] device: deko3D, %u recorders, %u image and %u sampler descriptors\n", dd->ncmd, IMAGE_DESCS,
           SAMPLER_DESCS);
    return &dd->base;
}

#endif /* PS3RECOMP_RSX_HAL && __SWITCH__ */
