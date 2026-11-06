/* nx_sdl.h -- SDL2 on this console: what ps3recomp's cellPad and cellAudio stand on. */

#ifndef NX_SDL_H
#define NX_SDL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char version[16];           /* the SDL2 linked in */
    uint32_t init_us;           /* the game controller and audio subsystems, started as cellPad and cellAudio do */
    int controllers;            /* game controllers SDL sees at the start */
    char controller[64];        /* the first one's name */
    const char *audio_driver;
    int audio_freq;             /* what the device gave for cellAudio's request: 48 kHz, float, stereo, 256 frames */
    int audio_channels;
    int audio_samples;
    bool audio_float;
    uint32_t audio_rate;        /* frames a second the device took from the queue, measured */
    const char *failed;         /* the step that failed, or NULL */
} NxSdlProbe;

/* Stage 5: starts SDL2's game controllers and audio the way the runtime does, opens the audio
 * device with cellAudio's request, and plays a short tone through its queue while timing it.
 * The game controllers stay open for nx_sdl_pad_line. */
void nx_sdl_probe(NxSdlProbe *out);

/* The first controller's buttons and sticks, as SDL names them, for the screen. Logs each
 * button the first time it is pressed, so the log shows how the console's buttons map. */
void nx_sdl_pad_line(char *out, size_t size);

void nx_sdl_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_SDL_H */
