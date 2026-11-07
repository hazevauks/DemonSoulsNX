/* rsxc_dk.c -- the portable renderer's core (ps3recomp's libs/video/rsxcore), compiled for the
 * deko3D backend (gpu_dk.c), as rsxc_null.c and rsxc_vk.c compile it for theirs.
 *
 * The route RSX_BACKEND=deko3d (rsx_out.c), the Switch's default. What differs from the null
 * instance, all in the rc_be_* hooks below:
 *   - headless as far as the core knows: it opens no window of its own (SDL's would take the
 *     default window), and the backend's swap chain is on the default window, whatever window
 *     the core hands it;
 *   - the shaders are DKSH. The core's built-in passes come by name from the ones compiled at
 *     build time (src/gpu/shaders, docs/deko3d.md); the game's programs have none yet (stage
 *     12c), so their pipelines fail and the core leaves their draws out;
 *   - no cache files and no pipeline library: deko3D has no pipeline objects to keep.
 */
#if defined(PS3RECOMP_RSX_HAL) && defined(__SWITCH__)

#include "../../third_party/ps3recomp/runtime/platform/win32_compat.h"
#include "rsxc_posix.h"
#include "rsxc_head.inc"
#include "gpu_dk_cmd.h"
#include "gpu_shader.h"

#include "blit_fsh_dksh.h"
#include "blit_vsh_dksh.h"

static gpu_device* rc_be_create(const gpu_device_desc* dd) { return gpu_dk_create(dd); }

/* The core opens no window: the swap chain is on the default one (gpu_dk.c). */
static const int rc_be_headless = 1;

/* A frame buffer that cannot be made does not fail the start (as on the null route): there is no
 * renderer to fall back on, and what does start says more in the log than nothing. */
static const int rc_be_start_strict = 0;

static int rc_be_breadcrumbs(void) { return 0; }
static const char rc_be_lost_hint[] = "";

static const char* rc_be_lost_text(u32 reason)
{
    return reason == GPU_DK_LOST_QUEUE ? "the GPU queue went into its error state (a fault in the GPU's work)"
                                       : "an unknown reason";
}
static void rc_be_lose_device(gpu_device* d, int frames)
{
    fprintf(stderr, "[dk] RSX_LOSE_DEVICE_AT_FRAME: noting the device lost at frame %d%c", frames, 10);
    gpu_dk_lose(d);
}
static const u32 rc_be_surface_lost = 0;

/* The menu's renderer has nothing to draw with here. */
static void* rc_be_device(gpu_device* d) { (void)d; return NULL; }
static void* rc_be_queue(gpu_device* d)  { (void)d; return NULL; }
static void* rc_be_image(gpu_device* d, gpu_texture t) { (void)d; (void)t; return NULL; }

static const int rc_be_cache_files = 0;
static const char rc_be_lib_dir[] = "deko3d";   /* (never used: no pipeline library) */
static const char* rc_be_lib_file(gpu_device* d) { (void)d; return "pipelines.lib"; }
static int rc_be_cache_put(gpu_sc_stage stage, const char* hlsl) { (void)stage; (void)hlsl; return 0; }

/* The shaders: the built-in passes' DKSH by name. */
static const uint32_t rc_be_shader_code = GPU_CODE_DKSH;
static const struct {
    const char* name;
    const uint8_t* code;
    const size_t* size;
} k_rc_dk_builtin[] = {
    {"blit_vs", blit_vsh_dksh, &blit_vsh_dksh_size},
    {"blit_ps", blit_fsh_dksh, &blit_fsh_dksh_size},
};
static int rc_be_sc_compile(gpu_sc_stage stage, const char* src, size_t n, const char* name, const char* entry,
                            unsigned flags, const gpu_layout_desc* layout, gpu_sc_blob* out, char* msg, size_t cap)
{
    (void)stage; (void)src; (void)n; (void)entry; (void)flags; (void)layout;
    for (size_t i = 0; name && i < sizeof k_rc_dk_builtin / sizeof k_rc_dk_builtin[0]; i++)
        if (!strcmp(name, k_rc_dk_builtin[i].name)) {
            out->data = k_rc_dk_builtin[i].code;
            out->size = *k_rc_dk_builtin[i].size;
            out->impl = NULL;   /* (embedded: nothing to free) */
            if (msg && cap) msg[0] = 0;
            return 1;
        }
    if (msg && cap)
        snprintf(msg, cap, "no DKSH for %s yet: the game's programs come with stage 12c", name ? name : "a program");
    return 0;
}
static int rc_be_sc_load(gpu_sc_stage stage, u64 key, gpu_sc_blob* out)
{
    (void)stage; (void)key; (void)out;
    return 0;
}

/* No pipeline library: no record, no name. */
typedef struct rc_be_pso_desc { u32 none; } rc_be_pso_desc;
static u64 rc_be_layout_hash(gpu_device* d, gpu_layout l) { (void)d; (void)l; return 0; }
static u64 rc_be_pso_name(gpu_device* d, const gpu_pipeline_desc* pd, u64 layout_hash)
{
    (void)d; (void)pd; (void)layout_hash;
    return 0;
}
static int rc_be_pso_record(gpu_device* d, const gpu_pipeline_desc* pd, u64 layout_hash, u64* name,
                            rc_be_pso_desc* rec, u32* nel, const void* code[2], u32 len[2])
{
    (void)d; (void)pd; (void)layout_hash; (void)name; (void)rec; (void)nel; (void)code; (void)len;
    return 0;
}
static int rc_be_pso_unrecord(const rc_be_pso_desc* rec, u32 nel, gpu_pipeline_desc* pd)
{
    (void)rec; (void)nel; (void)pd;
    return 0;
}

/* After each present: counted, and the backend's line every 300 (about 10 seconds). */
static u32 s_rc_dk_presents;
static void rc_be_presented(gpu_device* d)
{
    if (++s_rc_dk_presents % 300 == 1)
        gpu_dk_report(d, s_rc_dk_presents);
}

/* The frame slot after an acquire: the image's index, as on D3D12 (the swap chain has
 * FRAME_COUNT images, handed out in turn). */
static u32 rc_be_frame_slot(u32 prev, u32 image) { (void)prev; return image; }

#include "rsxc_core.inc"

#include "rsx_out.h"

#define RSXC_ROUTE_V_(name, legacy, params, args)      .name = rc_##name,
#define RSXC_ROUTE_R_(ret, name, legacy, params, args) .name = rc_##name,
const rsx_route rsxc_route_dk = {
    .name = "deko3d",
    RSX_OUT_FUNCS(RSXC_ROUTE_V_, RSXC_ROUTE_R_)
};
#undef RSXC_ROUTE_V_
#undef RSXC_ROUTE_R_

const rsx_route_gpu rsxc_route_gpu_dk = { rc_set_overlay_gpu, rc_gpu_device, rc_overlay_gpu_flush };

#endif /* PS3RECOMP_RSX_HAL && __SWITCH__ */
