/* nx_game_io.h -- the game's file reads and writes, counted by where they go (nx_game_io.c). */

#ifndef NX_GAME_IO_H
#define NX_GAME_IO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NX_IO_DISC, NX_IO_CACHE, NX_IO_SAVES, NX_IO_KINDS } NxIoKind;

/* Logs the totals so far, for the disc, the cache and the saves, when they have changed. */
void nx_game_io_report(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_GAME_IO_H */
