/* nx_sdl_probe.c -- stage 5: SDL2's game controllers and audio on this console.
 *
 * ps3recomp's cellPad reads the pads through SDL2's game controller API, and its cellAudio plays
 * through an SDL2 audio queue (libs/input/cellPad.c, libs/audio/cellAudio.c): on the Switch,
 * switch-sdl2's own drivers, over libnx's pad and audio services. SDL2's video is never started,
 * as deko3D owns the window. This starts both subsystems the way those modules do, opens the
 * audio device with cellAudio's request, and times how fast the device takes its queue: cellAudio
 * paces the game's audio clock on that rate.
 *
 * The first console test played cellAudio's 256-frame buffers at 25,600 frames a second, not
 * 48,000. switch-sdl2's driver (src/audio/switch) gives the audio renderer two buffers and waits
 * for each to start playing before it takes the next from the queue; a 5.3 ms buffer is shorter
 * than that round trip, so the voice runs dry between buffers. So the rate is measured at four
 * buffer sizes, and a tone played at 256 frames and at the smallest size that keeps up, for the
 * ear to compare.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <switch.h>
#include <zlib.h>

#include "nx_log.h"
#include "nx_sdl.h"

#define AUDIO_FREQ      48000       /* cellAudio's CELL_AUDIO_SAMPLE_RATE */
#define AUDIO_BLOCK     256         /* and CELL_AUDIO_BLOCK_SAMPLES */
#define AUDIO_FULL_RATE 47500       /* within 1% of 48 kHz */
#define FRAME_BYTES     (2 * sizeof(float))
#define QUEUE_MS        1000        /* silence queued for a measurement: more than it can take */
#define START_MS        100         /* the device fills its two buffers first */
#define MEASURE_MS      500
#define TONE_MS         400
#define TONE_GAP_MS     300
#define TONE_HZ         440.0f
#define TONE_LEVEL      0.08f
#define FADE_FRAMES     480         /* 10 ms in and out, so the tone does not click */
#define TRIGGER_DOWN    16000       /* ZL and ZR are digital: SDL gives 0 or 32767 */

static const int k_audio_sizes[NX_SDL_AUDIO_SIZES] = {AUDIO_BLOCK, 512, 1024, 2048};

static SDL_GameController *s_pad;
static uint32_t s_logged;           /* buttons, and the two triggers above them, already logged */

static uint64_t now_us(void)
{
    return armTicksToNs(armGetSystemTick()) / 1000;
}

static void sleep_ms(unsigned ms)
{
    svcSleepThread((int64_t)ms * 1000000);
}

static void open_first_pad(void)
{
    if (s_pad)
        return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i) || !(s_pad = SDL_GameControllerOpen(i)))
            continue;
        const char *name = SDL_GameControllerName(s_pad);
        nx_log("sdl: controller %d: %s, type %d, player %d\n", i, name ? name : "?",
               (int)SDL_GameControllerGetType(s_pad), SDL_GameControllerGetPlayerIndex(s_pad));
        return;
    }
}

static SDL_AudioDeviceID open_audio(int samples, SDL_AudioSpec *have)
{
    SDL_AudioSpec want;
    memset(&want, 0, sizeof(want));
    want.freq = AUDIO_FREQ;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = (uint16_t)samples;
    SDL_AudioDeviceID device = SDL_OpenAudioDevice(NULL, 0, &want, have, 0);
    if (!device)
        nx_log("sdl: opening the audio device for %d frames a buffer failed: %s\n", samples, SDL_GetError());
    return device;
}

/* Frames a second the device takes from its queue, from the first to the last time the queue
 * shrinks within the measurement, so the size of each take does not round the figure. */
static uint32_t measure_rate(SDL_AudioDeviceID device)
{
    static const float silence[AUDIO_BLOCK * 2];
    for (int i = 0; i < AUDIO_FREQ * QUEUE_MS / 1000 / AUDIO_BLOCK; i++)
        if (SDL_QueueAudio(device, silence, sizeof(silence)) < 0)
            return 0;

    SDL_PauseAudioDevice(device, 0);
    sleep_ms(START_MS);
    uint32_t last = SDL_GetQueuedAudioSize(device), first_left = 0, last_left = 0;
    uint64_t first_at = 0, last_at = 0, end = now_us() + MEASURE_MS * 1000;
    while (now_us() < end) {
        sleep_ms(1);
        uint32_t left = SDL_GetQueuedAudioSize(device);
        if (left < last) {
            uint64_t at = now_us();
            if (!first_at) {
                first_at = at;
                first_left = left;
            }
            last_at = at;
            last_left = left;
        }
        last = left;
    }
    if (last_at <= first_at)
        return 0;
    return (uint32_t)((uint64_t)(first_left - last_left) / FRAME_BYTES * 1000000 / (last_at - first_at));
}

static void play_tone(int samples)
{
    SDL_AudioSpec have;
    SDL_AudioDeviceID device = open_audio(samples, &have);
    if (!device)
        return;
    const int frames = AUDIO_FREQ * TONE_MS / 1000;
    float *tone = malloc((size_t)frames * FRAME_BYTES);
    if (tone) {
        for (int i = 0; i < frames; i++) {
            float level = TONE_LEVEL;
            if (i < FADE_FRAMES)
                level *= (float)i / FADE_FRAMES;
            else if (frames - i < FADE_FRAMES)
                level *= (float)(frames - i) / FADE_FRAMES;
            float sample = level * sinf(2.0f * (float)M_PI * TONE_HZ * (float)i / AUDIO_FREQ);
            tone[2 * i] = sample;
            tone[2 * i + 1] = sample;
        }
        SDL_QueueAudio(device, tone, (uint32_t)(frames * FRAME_BYTES));
        free(tone);
        SDL_PauseAudioDevice(device, 0);
        for (int waited = 0; SDL_GetQueuedAudioSize(device) > 0 && waited < 3 * TONE_MS; waited += 10)
            sleep_ms(10);
        sleep_ms(2 * 1000 * samples / AUDIO_FREQ + 20);  /* what is left in the device's two buffers */
    }
    SDL_CloseAudioDevice(device);
}

static void probe_audio(NxSdlProbe *out)
{
    out->audio_driver = SDL_GetCurrentAudioDriver();
    for (int i = 0; i < NX_SDL_AUDIO_SIZES; i++) {
        SDL_AudioSpec have;
        SDL_AudioDeviceID device = open_audio(k_audio_sizes[i], &have);
        if (!device) {
            out->failed = "SDL audio device";
            return;
        }
        if (i == 0) {
            out->audio_freq = have.freq;
            out->audio_channels = have.channels;
            out->audio_float = have.format == AUDIO_F32SYS;
            nx_log("sdl: audio driver %s: %d Hz, %d channels, format 0x%04x\n",
                   out->audio_driver ? out->audio_driver : "?", have.freq, have.channels, have.format);
        }
        out->audio_sizes[i] = have.samples;
        out->audio_rates[i] = measure_rate(device);
        SDL_CloseAudioDevice(device);
        nx_log("sdl: %d frames a buffer: the device took %u frames a second\n", have.samples, out->audio_rates[i]);
        if (!out->audio_full && out->audio_rates[i] >= AUDIO_FULL_RATE)
            out->audio_full = have.samples;
    }
    if (!out->audio_rates[0]) {
        out->failed = "SDL audio playback";
        return;
    }

    int other = out->audio_full && out->audio_full != AUDIO_BLOCK ? out->audio_full : k_audio_sizes[NX_SDL_AUDIO_SIZES - 1];
    if (out->audio_full)
        nx_log("sdl: 48000 frames a second from %d frames a buffer; cellAudio asks for %d\n", out->audio_full, AUDIO_BLOCK);
    else
        nx_log("sdl: no buffer size reached 48000 frames a second\n");
    nx_log("sdl: a tone at %d frames a buffer, then one at %d\n", AUDIO_BLOCK, other);
    play_tone(AUDIO_BLOCK);
    sleep_ms(TONE_GAP_MS);
    play_tone(other);
}

void nx_sdl_probe(NxSdlProbe *out)
{
    memset(out, 0, sizeof(*out));
    SDL_version linked, built;
    SDL_GetVersion(&linked);
    SDL_VERSION(&built);
    snprintf(out->version, sizeof(out->version), "%u.%u.%u", linked.major, linked.minor, linked.patch);
    nx_log("libs: SDL %s (built against %u.%u.%u), zlib %s\n", out->version, built.major, built.minor,
           built.patch, zlibVersion());

    uint64_t start = now_us();
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
        nx_log("sdl: game controllers failed: %s\n", SDL_GetError());
        out->failed = "SDL game controllers";
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        nx_log("sdl: audio failed: %s\n", SDL_GetError());
        out->failed = "SDL audio";
        return;
    }
    out->init_us = (uint32_t)(now_us() - start);

    for (int i = 0; i < SDL_NumJoysticks(); i++)
        out->controllers += SDL_IsGameController(i) ? 1 : 0;
    nx_log("sdl: started in %u us: %d joysticks, %d game controllers\n", out->init_us, SDL_NumJoysticks(),
           out->controllers);
    open_first_pad();
    if (s_pad) {
        const char *name = SDL_GameControllerName(s_pad);
        snprintf(out->controller, sizeof(out->controller), "%s", name ? name : "?");
    }

    probe_audio(out);
}

static void append(char *out, size_t size, size_t *used, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

static void append(char *out, size_t size, size_t *used, const char *format, ...)
{
    if (*used >= size)
        return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out + *used, size - *used, format, args);
    va_end(args);
    if (n > 0)
        *used += (size_t)n < size - *used ? (size_t)n : size - *used - 1;
}

static void note_first_press(unsigned bit, const char *what, const char *name)
{
    if (s_logged & (1u << bit))
        return;
    s_logged |= 1u << bit;
    nx_log("sdl: %s %s\n", what, name);
}

void nx_sdl_pad_line(char *out, size_t size)
{
    size_t used = 0;
    out[0] = '\0';
    SDL_GameControllerUpdate();
    if (s_pad && !SDL_GameControllerGetAttached(s_pad)) {
        nx_log("sdl: controller detached\n");
        SDL_GameControllerClose(s_pad);
        s_pad = NULL;
    }
    open_first_pad();
    if (!s_pad) {
        append(out, size, &used, "no controller");
        return;
    }

    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
        if (!SDL_GameControllerGetButton(s_pad, (SDL_GameControllerButton)b))
            continue;
        const char *name = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)b);
        note_first_press((unsigned)b, "button", name);
        append(out, size, &used, "%s ", name);
    }
    int zl = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    int zr = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (zl > TRIGGER_DOWN) {
        note_first_press(SDL_CONTROLLER_BUTTON_MAX, "trigger", "lefttrigger");
        append(out, size, &used, "lefttrigger ");
    }
    if (zr > TRIGGER_DOWN) {
        note_first_press(SDL_CONTROLLER_BUTTON_MAX + 1, "trigger", "righttrigger");
        append(out, size, &used, "righttrigger ");
    }
    if (used == 0)
        append(out, size, &used, "(none) ");

    append(out, size, &used, "| L %+.2f %+.2f  R %+.2f %+.2f",
           SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0,
           SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0,
           SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0,
           SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0);
}

void nx_sdl_shutdown(void)
{
    if (s_pad) {
        SDL_GameControllerClose(s_pad);
        s_pad = NULL;
    }
    SDL_Quit();
}
