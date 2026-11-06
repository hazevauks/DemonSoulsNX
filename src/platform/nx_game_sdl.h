/* nx_game_sdl.h -- the game's sound and controls on the Switch (nx_game_sdl.c). */

#ifndef NX_GAME_SDL_H
#define NX_GAME_SDL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Logs what the game's mixer has queued since the last call: frames a second, the loudest
 * sample, and how much is waiting. Nothing until cellAudio opens the device. */
void nx_game_sdl_report(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_GAME_SDL_H */
