/* nx_game_threads.h -- detached threads in the game (nx_game_threads.c). */

#ifndef NX_GAME_THREADS_H
#define NX_GAME_THREADS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned detached;          /* detached threads not ended yet, or not joined yet */
    unsigned freed;             /* detached threads that ended and were given back */
    unsigned peak;              /* the most detached threads at once */
    unsigned lost;              /* detached threads the list had no room for: never given back */
} NxThreadStats;

void nx_game_threads_stats(NxThreadStats *out);

/* DESNX_CPU_REPORT=1: the busiest threads over the last window_ns, in the log */
void nx_game_threads_cpu_report(uint64_t window_ns);

/* DESNX_PROFILE=1: sample the busiest threads' pc and lr into SD_LOG/profile/<n>.bin */
void nx_game_threads_profile_start(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_GAME_THREADS_H */
