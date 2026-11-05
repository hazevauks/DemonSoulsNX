/* nx_cpu.h -- the cores the game's threads get, and how fast they wake each other. */

#ifndef NX_CPU_H
#define NX_CPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t core_mask;             /* the cores this process may run threads on */
    uint64_t priority_mask;         /* the thread priorities it may use */
    unsigned cores;
    uint32_t core_ms[4];            /* the same work on each core alone; 0 = not allowed */
    uint32_t wake_cross_ns;         /* one wake through address arbitration, the other thread on another core */
    uint32_t wake_same_ns;          /* the same with both threads on one core */
    uint32_t condvar_cross_ns;      /* one wake through libnx's CondVar, which pthreads use */
    uint32_t sleep_us, sleep_worst_us;  /* svcSleepThread for 1 ms */
    uint32_t yield_ns;
} NxCpuProbe;

/* Stage 3: per-core speed, wake latency and sleep accuracy, from worker threads. */
void nx_cpu_probe(NxCpuProbe *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_CPU_H */
