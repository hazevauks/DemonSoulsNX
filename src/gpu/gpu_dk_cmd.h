/* gpu_dk_cmd.h -- the deko3D backend's commands, for the core's draw loop (rsxc_dk.c).
 *
 * Every command of gpu_cmd_ops.h as a static inline function, each one call into the
 * backend's recorder (gpu_dk.c), and the per-draw device calls (GPU_FAST_FUNCS) the same way:
 * the shape of the null backend's gpu_null_cmd.h. The calls themselves are out of line, as the
 * backend keeps its recorder's state (the bound layout, tables and push constants) in gpu_dk.c.
 * docs/deko3d.md says what each part becomes on deko3D.
 */
#ifndef DESNX_GPU_DK_CMD_H
#define DESNX_GPU_DK_CMD_H

#if defined(PS3RECOMP_RSX_HAL)

#include "gpu.h"
#include "gpu_cmd_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The code kind of a DKSH shader (gpu_shader_desc.code): uam's output, as the built-in passes
 * have it. */
#define GPU_CODE_DKSH 16u

/* gpu_lost's reason once the queue has gone into its error state. */
#define GPU_DK_LOST_QUEUE 0xDE3D0001u

gpu_device* gpu_dk_create(const gpu_device_desc* desc);
/* RSX_LOSE_DEVICE_AT_FRAME: the device noted lost, as a GPU fault would leave it. */
void gpu_dk_lose(gpu_device* d);
/* One line on stdout: presents so far, memory in use, objects made. */
void gpu_dk_report(gpu_device* d, uint32_t presents);

#define GPU__DK_DECL(name, form, ret, params, args) ret gpu_dk_cmd_##name params;
GPU_CMD_OPS(GPU__DK_DECL)
#undef GPU__DK_DECL
#define GPU__DK_FAST_DECL(name, form, ret, params, args) ret gpu_dk_fast_##name params;
GPU_FAST_FUNCS(GPU__DK_FAST_DECL)
#undef GPU__DK_FAST_DECL

#define GPU__DK_CMD_V(name, ret, params, args) \
    static inline void gpu_cmd_##name params { gpu_dk_cmd_##name args; }
#define GPU__DK_CMD_R(name, ret, params, args) \
    static inline ret gpu_cmd_##name params { return gpu_dk_cmd_##name args; }
#define GPU__DK_CMD(name, form, ret, params, args) GPU__DK_CMD_##form(name, ret, params, args)
GPU_CMD_OPS(GPU__DK_CMD)
#undef GPU__DK_CMD
#undef GPU__DK_CMD_V
#undef GPU__DK_CMD_R

#define GPU__DK_FAST_V(name, ret, params, args) \
    static inline void gpu_fast_##name params { gpu_dk_fast_##name args; }
#define GPU__DK_FAST_R(name, ret, params, args) \
    static inline ret gpu_fast_##name params { return gpu_dk_fast_##name args; }
#define GPU__DK_FAST(name, form, ret, params, args) GPU__DK_FAST_##form(name, ret, params, args)
GPU_FAST_FUNCS(GPU__DK_FAST)
#undef GPU__DK_FAST
#undef GPU__DK_FAST_V
#undef GPU__DK_FAST_R

GPU_CMD_CHECK(dk);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_RSX_HAL */
#endif /* DESNX_GPU_DK_CMD_H */
