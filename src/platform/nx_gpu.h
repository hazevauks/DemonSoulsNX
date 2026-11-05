/* nx_gpu.h -- deko3D on this console. */

#ifndef NX_GPU_H
#define NX_GPU_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool shaders_ok;            /* the programs uam built load as a vertex and a fragment shader */
    unsigned frames;            /* frames drawn and shown */
    uint32_t frame_us;          /* time per frame, waits for the display included */
    uint32_t worst_frame_us;
    bool queue_ok;              /* the queue ended without an error */
    const char *failed;         /* the step that failed, or NULL */
} NxGpuProbe;

/* Stage 3: two seconds of a triangle through deko3D, before the text console takes the window. */
void nx_gpu_probe(NxGpuProbe *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_GPU_H */
