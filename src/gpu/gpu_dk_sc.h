/* gpu_dk_sc.h -- the game's shaders on the console: the spv target's SPIR-V to DKSH
 * (gpu_dk_sc.cpp, docs/deko3d.md). */
#ifndef DESNX_GPU_DK_SC_H
#define DESNX_GPU_DK_SC_H

#include <stddef.h>
#include <stdint.h>

#include "gpu.h"
#include "gpu_shader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A module the spv target made for `layout` (never NULL here: the guest programs' layout is
 * gpu_spv_guest_layout()) as DKSH, malloc'd into *out (for free()). 1, or 0 with msg. */
int gpu_dk_sc_dksh(gpu_sc_stage stage, const uint32_t* words, size_t nwords, const gpu_layout_desc* layout,
                   void** out, size_t* out_size, char* msg, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* DESNX_GPU_DK_SC_H */
