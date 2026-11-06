/* nx_game_threads.h -- detached threads in the game (nx_game_threads.c). */

#ifndef NX_GAME_THREADS_H
#define NX_GAME_THREADS_H

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

#ifdef __cplusplus
}
#endif

#endif /* NX_GAME_THREADS_H */
