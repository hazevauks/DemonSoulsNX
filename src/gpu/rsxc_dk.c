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

#include "gpu_shader_spv.h"
#include "gpu_dk_sc.h"

#include <ctype.h>
#include <sys/stat.h>

#include "blit_fsh_dksh.h"
#include "blit_vsh_dksh.h"

const char* rsx_d3d12_shader_cache_path(void);   /* the port's (src/platform/nx_port_glue.c) */

/* The device, and the spv target's post-passes for it: NoContraction and the invariant
 * position, no float controls (GLSL has none to carry them), and the vertex fetch kept inside
 * t20, as deko3D reads past a storage buffer's end as it finds it. */
static gpu_device* rc_be_create(const gpu_device_desc* dd)
{
    gpu_device* const d = gpu_dk_create(dd);
    if (d)
        gpu_sc_spv_set_options((GPU_SC_SPV_DEFAULT & ~(GPU_SC_SPV_FLOAT_PRESERVE | GPU_SC_SPV_DENORM_PRESERVE)) |
                               GPU_SC_SPV_FETCH_CLAMP);
    return d;
}

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
/* The DKSH cache on the SD card: <shader cache>/dk/<key>.dksh, the key FNV-1a 64 over what the
 * code depends on: this file's version of the chain, the spv target's tag (its compilers, their
 * versions, its edits and the layout), the stage, the entry point and the text. */
static u64 dk_fnv(u64 h, const void* p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= ((const u8*)p)[i]; h *= 1099511628211ull; }
    return h;
}
static int dk_cache_file(gpu_sc_stage stage, const char* src, size_t n, const char* entry, const gpu_layout_desc* layout,
                         char* path, size_t cap)
{
    const char* dir = rsx_d3d12_shader_cache_path();
    if (!dir || !dir[0]) return 0;
    static int made;
    char sub[512];
    snprintf(sub, sizeof sub, "%s/dk", dir);
    if (!made) {
        mkdir(dir, 0777);
        mkdir(sub, 0777);
        made = 1;
    }
    char tag[512];
    gpu_spv_tag(layout, tag, sizeof tag);
    u64 h = 14695981039346656037ull;
    static const char version[] = "dksh 2: spirv-cross glsl 460 flip-y, uam 1.1.0 level 3, mesa once past 20 KB;";
    h = dk_fnv(h, version, sizeof version - 1);
    h = dk_fnv(h, tag, strlen(tag));
    const u32 st = (u32)stage;
    h = dk_fnv(h, &st, sizeof st);
    if (entry) h = dk_fnv(h, entry, strlen(entry));
    h = dk_fnv(h, src, n);
    snprintf(path, cap, "%s/%016llx.dksh", sub, (unsigned long long)h);
    return 1;
}

static int dk_cache_read(const char* path, gpu_sc_blob* out)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* p = n > 0 ? malloc((size_t)n) : NULL;
    const int ok = p && fread(p, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(p); return 0; }
    out->data = p;
    out->size = (size_t)n;
    out->impl = p;   /* (gpu_sc_blob_release frees it) */
    return 1;
}

static void dk_cache_write(const char* path, const void* p, size_t n)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    const int ok = fwrite(p, 1, n, f) == n;
    if (fclose(f) == 0 && ok) rename(tmp, path);
    else remove(tmp);
}

/* What the compiles cost, every 50 (the log's). */
static volatile LONG s_dk_hits, s_dk_made, s_dk_failed;
static double s_dk_ms;
static void dk_count(int made, double ms)
{
    const LONG m = made ? InterlockedIncrement(&s_dk_made) : s_dk_made;
    if (made) s_dk_ms += ms;
    const LONG all = s_dk_made + s_dk_hits + s_dk_failed;
    if (all % 50 == 1)
        printf("[dk] shaders: %ld compiled (%.0f ms each on average), %ld from the SD card's cache, %ld failed\n",
               (long)m, m ? s_dk_ms / (double)m : 0.0, (long)s_dk_hits, (long)s_dk_failed);
}

static char* dk_fetch_loops(const char* src, size_t n);
static int dk_compile(gpu_sc_stage stage, const char* src, size_t n, const char* name, const char* entry,
                      unsigned flags, const gpu_layout_desc* layout, gpu_sc_blob* out, char* msg, size_t cap);

static int rc_be_sc_compile(gpu_sc_stage stage, const char* src, size_t n, const char* name, const char* entry,
                            unsigned flags, const gpu_layout_desc* layout, gpu_sc_blob* out, char* msg, size_t cap)
{
    for (size_t i = 0; name && i < sizeof k_rc_dk_builtin / sizeof k_rc_dk_builtin[0]; i++)
        if (!strcmp(name, k_rc_dk_builtin[i].name)) {
            out->data = k_rc_dk_builtin[i].code;
            out->size = *k_rc_dk_builtin[i].size;
            out->impl = NULL;   /* (embedded: nothing to free) */
            if (msg && cap) msg[0] = 0;
            return 1;
        }
    if (!src) return 0;
    /* everything else, the core's other built-in passes too, through the chain */
    char* edited = dk_fetch_loops(src, n);
    if (edited) {
        src = edited;
        n = strlen(edited);
    }
    const int rc = dk_compile(stage, src, n, name, entry, flags, layout, out, msg, cap);
    free(edited);
    return rc;
}

/* The vertex fetch of a guest vertex program that reads its own vertices (rsx_vp_decompiler.c's
 * fetch mode): it fetched all 16 attributes, unrolled, each with its 4 components times every
 * format's loads unrolled too. That made the SPIR-V 25 times its usual size and kept all 16
 * attributes live beside the program's 32 registers and 16 outputs, and uam took 66 to 89
 * seconds on one program. Here only the attributes the program reads are fetched, one line
 * each, and the components' loop stays a loop. The same values either way. NULL when the text
 * is not a fetch program. */
static char* dk_fetch_loops(const char* src, size_t n)
{
    static const char k_all[] = "[unroll] for (uint _k = 0u; _k < 16u; _k++) v[_k] = vf_fetch(_k, input.vid);";
    char used[16 * 48] = "";
    const char* loop = NULL;
    for (size_t p = 0; p + sizeof k_all - 1 <= n && !loop; p++)
        if (!memcmp(src + p, k_all, sizeof k_all - 1)) loop = src + p;
    if (loop) {   /* the attributes read after it: v[0] to v[15] */
        unsigned mask = 0;
        for (const char* p = loop + sizeof k_all - 1; p + 3 < src + n; p++)
            if (p[0] == 'v' && p[1] == '[' && p[2] >= '0' && p[2] <= '9' && (p == src || !isalnum((unsigned char)p[-1]))) {
                unsigned k = (unsigned)(p[2] - '0');
                if (p[3] >= '0' && p[3] <= '9') k = k * 10u + (unsigned)(p[3] - '0');
                if (k < 16) mask |= 1u << k;
            }
        size_t u = 0;
        for (unsigned k = 0; k < 16; k++)
            if (mask & (1u << k))
                u += (size_t)snprintf(used + u, sizeof used - u, "v[%u] = vf_fetch(%uu, input.vid); ", k, k);
        if (!u) snprintf(used, sizeof used, "v[0] = float4(0, 0, 0, 1);");
    }
    const char* const from[] = {loop ? k_all : NULL, "[unroll] for (uint c = 0u; c < 4u; c++) {"};
    const char* const to[] = {used, "[loop] for (uint c = 0u; c < 4u; c++) {"};
    if (!loop) return NULL;
    char* t = NULL;
    for (int i = 0; i < 2; i++) {
        const char* text = t ? t : src;
        const size_t len = t ? strlen(t) : n;
        const char* at = NULL;
        const size_t fl = strlen(from[i]);
        for (size_t p = 0; p + fl <= len && !at; p++)
            if (!memcmp(text + p, from[i], fl)) at = text + p;
        if (!at) continue;
        const size_t tl = strlen(to[i]), off = (size_t)(at - text);
        char* nt = (char*)malloc(len - fl + tl + 1);
        if (!nt) break;
        memcpy(nt, text, off);
        memcpy(nt + off, to[i], tl);
        memcpy(nt + off + tl, at + fl, len - off - fl);
        nt[len - fl + tl] = 0;
        free(t);
        t = nt;
    }
    return t;
}

static int dk_compile(gpu_sc_stage stage, const char* src, size_t n, const char* name, const char* entry,
                      unsigned flags, const gpu_layout_desc* layout, gpu_sc_blob* out, char* msg, size_t cap)
{
    const gpu_layout_desc* const lay = layout ? layout : gpu_spv_guest_layout();
    char path[600];
    const int cached = dk_cache_file(stage, src, n, entry, lay, path, sizeof path);
    static int read_cache = -1;   /* DESNX_DK_CACHE=0: compile every program again (still written) */
    if (read_cache < 0) {
        const char* e = getenv("DESNX_DK_CACHE");
        read_cache = !(e && *e == '0');
    }
    if (cached && read_cache && dk_cache_read(path, out)) {
        InterlockedIncrement(&s_dk_hits);
        return 1;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    gpu_sc_blob spv;
    memset(&spv, 0, sizeof spv);
    if (gpu_sc_compile_layout(GPU_SC_SPV, stage, src, n, name, entry, layout, flags | GPU_SC_NO_CACHE, &spv, msg, cap) !=
        GPU_SC_OK) {
        InterlockedIncrement(&s_dk_failed);
        static int said_spv;
        if (said_spv++ < 8) printf("[dk] shader %s: the spv target refused it: %s\n", name ? name : "?", msg ? msg : "?");
        dk_count(0, 0);
        return 0;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const double spv_ms = (double)(ts.tv_sec - t0.tv_sec) * 1e3 + (double)(ts.tv_nsec - t0.tv_nsec) / 1e6;
    if (spv_ms > 500.0 || getenv("DESNX_DK_SHADER_TIMES"))
        printf("[dk] shader %s: glslang and SPIRV-Tools %.0f ms (%zu bytes of HLSL)\n", name ? name : "?", spv_ms, n);
    void* dksh = NULL;
    size_t size = 0;
    const int ok = gpu_dk_sc_dksh(stage, (const uint32_t*)spv.data, spv.size / 4, lay, &dksh, &size, msg, cap);
    gpu_sc_blob_release(&spv);
    if (!ok) {
        InterlockedIncrement(&s_dk_failed);
        static int said;
        if (said++ < 8) printf("[dk] shader %s (%s) failed: %s\n", name ? name : "?", stage == GPU_SC_PS ? "ps" : "vs",
                               msg ? msg : "?");
        dk_count(0, 0);
        return 0;
    }
    if (cached) dk_cache_write(path, dksh, size);
    out->data = dksh;
    out->size = size;
    out->impl = dksh;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    dk_count(1, (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6);
    return 1;
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
