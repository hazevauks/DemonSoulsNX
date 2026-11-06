/* nx_game_sdl.c -- the game's sound and controls on the Switch: what ps3recomp's cellAudio and
 * cellPad ask of SDL2, adjusted where the Switch's SDL2 differs, and logged.
 *
 * Linked with -Wl,--wrap for each function below (CMakeLists.txt), so the runtime's calls come
 * here first and the vendored files stay as they are.
 *
 * Sound. cellAudio opens the device with 256 frames a buffer, the PS3's block. switch-sdl2's
 * driver keeps two buffers in the audio renderer and waits for each to start; at 256 frames it
 * played 25,600 frames a second, and cellAudio paces the game's audio clock on what the device
 * takes. From 1024 frames it keeps up (stage 5), so the device gets 1024. cellAudio already
 * takes a device buffer larger than its blocks (its s_sdl_chunk).
 *
 * Controls. switch-sdl2 lists eight controllers, one for each of the console's controller
 * slots, whether a controller is in it or not, and cellPad would report every one of them to
 * the game as a pad. Here only a slot with a controller in it is a game controller. SDL2's
 * first is player 1 together with the handheld Joy-Cons, so a Pro Controller, a pair of
 * Joy-Cons or the console in hand is the game's port 0. Its buttons are by position, as the
 * PS3's: B is Cross, A Circle, Y Square, X Triangle.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL_audio.h>
#include <SDL2/SDL_gamecontroller.h>
#include <SDL2/SDL_thread.h>
#include <switch.h>

#include "nx_game_sdl.h"
#include "nx_log.h"

#define AUDIO_SAMPLES_MIN   1024        /* the smallest buffer the Switch's SDL2 keeps up with */
#define CONTROLLERS         8           /* switch-sdl2's JOYSTICK_COUNT */
#define TRIGGER_DOWN        3000        /* where cellPad takes ZL and ZR for L2 and R2 */
#define STICK_MOVED         16000

SDL_AudioDeviceID __real_SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
                                             SDL_AudioSpec *obtained, int allowed_changes);
int __real_SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len);
int __real_SDL_SetThreadPriority(SDL_ThreadPriority priority);
SDL_bool __real_SDL_IsGameController(int joystick_index);
SDL_bool __real_SDL_GameControllerGetAttached(SDL_GameController *gamecontroller);
SDL_GameController *__real_SDL_GameControllerOpen(int joystick_index);
Uint8 __real_SDL_GameControllerGetButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button);
Sint16 __real_SDL_GameControllerGetAxis(SDL_GameController *gamecontroller, SDL_GameControllerAxis axis);

/* ---- sound ------------------------------------------------------------------------------- */

static SDL_AudioDeviceID s_device;
static uint32_t s_frame_bytes;
static bool s_float;
static int s_samples;
static uint64_t s_frames;           /* queued since the last report */
static uint32_t s_peak_bits;        /* the loudest sample since the last report, as float bits */
static uint64_t s_report_tick;

SDL_AudioDeviceID __wrap_SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
                                             SDL_AudioSpec *obtained, int allowed_changes)
{
    if (iscapture || !desired || desired->samples >= AUDIO_SAMPLES_MIN)
        return __real_SDL_OpenAudioDevice(device, iscapture, desired, obtained, allowed_changes);

    SDL_AudioSpec want = *desired;
    want.samples = AUDIO_SAMPLES_MIN;
    const SDL_AudioDeviceID id = __real_SDL_OpenAudioDevice(device, iscapture, &want, obtained, allowed_changes);
    const SDL_AudioSpec *got = obtained ? obtained : &want;
    nx_log("sound: %u frames a buffer asked for; the Switch's SDL2 keeps up from %d, so the device gets %u "
           "(%d Hz, %d channels)%s\n", (unsigned)desired->samples, AUDIO_SAMPLES_MIN, (unsigned)got->samples,
           got->freq, got->channels, id ? "" : ": it did not open");
    if (id) {
        s_device = id;
        s_samples = got->samples;
        s_frame_bytes = (uint32_t)(SDL_AUDIO_BITSIZE(got->format) / 8) * got->channels;
        s_float = SDL_AUDIO_ISFLOAT(got->format) && SDL_AUDIO_BITSIZE(got->format) == 32;
        s_report_tick = armGetSystemTick();
    }
    return id;
}

int __wrap_SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len)
{
    if (dev == s_device && s_frame_bytes) {
        __atomic_fetch_add(&s_frames, len / s_frame_bytes, __ATOMIC_RELAXED);
        if (s_float && data) {
            const float *sample = data;
            float peak = 0.0f;
            for (Uint32 i = 0; i < len / sizeof(float); i++) {
                const float v = sample[i] < 0 ? -sample[i] : sample[i];
                if (v > peak)
                    peak = v;
            }
            uint32_t bits;
            memcpy(&bits, &peak, sizeof bits);  /* positive floats order as their bits do */
            if (bits > __atomic_load_n(&s_peak_bits, __ATOMIC_RELAXED))
                __atomic_store_n(&s_peak_bits, bits, __ATOMIC_RELAXED);
        }
    }
    return __real_SDL_QueueAudio(dev, data, len);
}

/* SDL2's audio thread asks for SDL_THREAD_PRIORITY_TIME_CRITICAL as it starts. On the Switch
 * that changes nothing, and the thread stays at libnx's 59 with all of the game's. In the first
 * run past the SPURS wait the device stopped taking from its queue 15 seconds in, while the boot
 * made a thousand threads: most likely the thread went without the CPU long enough for the audio
 * renderer's voice to run dry, and switch-sdl2's driver waits for a buffer added to a stopped
 * voice to start playing, which it never does. A thread that asks for high priority gets 40
 * here, above the game's; this one sleeps on the renderer between buffers. */
#define HIGH_PRIORITY 40

int __wrap_SDL_SetThreadPriority(SDL_ThreadPriority priority)
{
    const int rc = __real_SDL_SetThreadPriority(priority);
    if (priority >= SDL_THREAD_PRIORITY_HIGH) {
        const Result set = svcSetThreadPriority(CUR_THREAD_HANDLE, HIGH_PRIORITY);
        static int s_told;
        if (!s_told++)
            nx_log("sound: a thread asked SDL2 for %s priority: it gets %d%s\n",
                   priority == SDL_THREAD_PRIORITY_TIME_CRITICAL ? "time-critical" : "high", HIGH_PRIORITY,
                   R_SUCCEEDED(set) ? "" : ", which the kernel refused");
        return R_SUCCEEDED(set) ? 0 : rc;
    }
    return rc;
}

void nx_game_sdl_report(void)
{
    if (!s_device)
        return;
    const uint64_t now = armGetSystemTick();
    const uint64_t us = armTicksToNs(now - s_report_tick) / 1000;
    s_report_tick = now;
    const uint64_t frames = __atomic_exchange_n(&s_frames, 0, __ATOMIC_RELAXED);
    const uint32_t bits = __atomic_exchange_n(&s_peak_bits, 0, __ATOMIC_RELAXED);
    float peak;
    memcpy(&peak, &bits, sizeof peak);
    const uint32_t queued = SDL_GetQueuedAudioSize(s_device) / (s_frame_bytes ? s_frame_bytes : 1);
    const SDL_AudioStatus status = SDL_GetAudioDeviceStatus(s_device);
    nx_log("sound: %llu frames a second from the game's mixer (48000 is right), the loudest sample %.3f, "
           "%u frames queued (%u ms) and two device buffers of %d (%d ms), the device %s\n",
           us ? (unsigned long long)(frames * 1000000 / us) : 0ull, (double)peak, queued, queued / 48,
           s_samples, 2 * s_samples / 48,
           status == SDL_AUDIO_PLAYING ? "playing" : status == SDL_AUDIO_PAUSED ? "paused" : "stopped");
}

/* ---- controls ---------------------------------------------------------------------------- */

static bool s_was_attached[CONTROLLERS];
static uint32_t s_pressed;          /* buttons, triggers and sticks already logged */

static uint32_t style_of(int index)
{
    if (index < 0 || index >= CONTROLLERS)
        return 0;
    uint32_t style = hidGetNpadStyleSet((HidNpadIdType)(HidNpadIdType_No1 + index));
    if (index == 0)
        style |= hidGetNpadStyleSet(HidNpadIdType_Handheld);
    return style;
}

static const char *controller_name(uint32_t style)
{
    if (style & HidNpadStyleTag_NpadHandheld)
        return "the Joy-Cons, in handheld mode";
    if (style & HidNpadStyleTag_NpadFullKey)
        return "a Pro Controller";
    if (style & HidNpadStyleTag_NpadJoyDual)
        return "a pair of Joy-Cons";
    if (style & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight))
        return "one Joy-Con";
    return "a controller";
}

static int index_of(SDL_GameController *gc)
{
    SDL_Joystick *joystick = gc ? SDL_GameControllerGetJoystick(gc) : NULL;
    return joystick ? (int)SDL_JoystickInstanceID(joystick) : -1;
}

SDL_bool __wrap_SDL_IsGameController(int joystick_index)
{
    return __real_SDL_IsGameController(joystick_index) && style_of(joystick_index) ? SDL_TRUE : SDL_FALSE;
}

SDL_GameController *__wrap_SDL_GameControllerOpen(int joystick_index)
{
    SDL_GameController *gc = __real_SDL_GameControllerOpen(joystick_index);
    if (gc && joystick_index >= 0 && joystick_index < CONTROLLERS) {
        const uint32_t style = style_of(joystick_index);
        nx_log("controls: controller slot %d has %s (style %#x)\n", joystick_index + 1, controller_name(style),
               style);
        s_was_attached[joystick_index] = true;
    }
    return gc;
}

SDL_bool __wrap_SDL_GameControllerGetAttached(SDL_GameController *gamecontroller)
{
    const int index = index_of(gamecontroller);
    const bool attached = __real_SDL_GameControllerGetAttached(gamecontroller) && style_of(index);
    if (index >= 0 && index < CONTROLLERS && s_was_attached[index] && !attached) {
        nx_log("controls: controller slot %d is empty now\n", index + 1);
        s_was_attached[index] = false;
    }
    return attached ? SDL_TRUE : SDL_FALSE;
}

/* The Switch's button, then the PS3's that cellPad gives the game for it. */
static const char *const button_names[SDL_CONTROLLER_BUTTON_MAX] = {
    [SDL_CONTROLLER_BUTTON_A] = "B (Cross)",
    [SDL_CONTROLLER_BUTTON_B] = "A (Circle)",
    [SDL_CONTROLLER_BUTTON_X] = "Y (Square)",
    [SDL_CONTROLLER_BUTTON_Y] = "X (Triangle)",
    [SDL_CONTROLLER_BUTTON_BACK] = "minus (Select)",
    [SDL_CONTROLLER_BUTTON_START] = "plus (Start)",
    [SDL_CONTROLLER_BUTTON_LEFTSTICK] = "the left stick pressed (L3)",
    [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = "the right stick pressed (R3)",
    [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = "L (L1)",
    [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = "R (R1)",
    [SDL_CONTROLLER_BUTTON_DPAD_UP] = "up (up)",
    [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = "down (down)",
    [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = "left (left)",
    [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = "right (right)",
};

static void first_time(unsigned bit, const char *what)
{
    if (!what || (__atomic_fetch_or(&s_pressed, 1u << bit, __ATOMIC_RELAXED) & (1u << bit)))
        return;
    nx_log("controls: %s reached the game\n", what);
}

Uint8 __wrap_SDL_GameControllerGetButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button)
{
    const Uint8 down = __real_SDL_GameControllerGetButton(gamecontroller, button);
    if (down && button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX)
        first_time((unsigned)button, button_names[button]);
    return down;
}

Sint16 __wrap_SDL_GameControllerGetAxis(SDL_GameController *gamecontroller, SDL_GameControllerAxis axis)
{
    const Sint16 value = __real_SDL_GameControllerGetAxis(gamecontroller, axis);
    const int far = value < 0 ? -value : value;
    switch (axis) {
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        if (value > TRIGGER_DOWN)
            first_time(24, "ZL (L2)");
        break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        if (value > TRIGGER_DOWN)
            first_time(25, "ZR (R2)");
        break;
    case SDL_CONTROLLER_AXIS_LEFTX:
    case SDL_CONTROLLER_AXIS_LEFTY:
        if (far > STICK_MOVED)
            first_time(26, "the left stick, moved,");
        break;
    case SDL_CONTROLLER_AXIS_RIGHTX:
    case SDL_CONTROLLER_AXIS_RIGHTY:
        if (far > STICK_MOVED)
            first_time(27, "the right stick, moved,");
        break;
    default:
        break;
    }
    return value;
}
