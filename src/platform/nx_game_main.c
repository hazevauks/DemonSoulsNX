/* nx_game_main.c -- the game NRO's start, before ps3recomp's boot (runtime/ppu/tests/boot_main.cpp).
 *
 * The game links with -Wl,--wrap=main: libnx's start code calls __wrap_main, which gives the
 * boot what a PC gives it and the Switch does not, then runs the boot's own main
 * (__real_main), unchanged:
 *
 *   - the log in SD_LOG, stdout included, and the PS3's drives on the SD card (nx_vfs);
 *   - the guest's 4 GB: nx_guest's window as vm_base before the boot looks for memory. Off
 *     Windows the boot maps 8 GB and keeps the 4 GB on a 4 GB boundary inside it; the shim's
 *     mmap refuses that here, and the boot goes on with the vm_base it finds. The shim's
 *     mprotect then commits in the window (src/hos/hos_compat.c);
 *   - the EBOOT.elf's path, the argument a PC's command line gives;
 *   - a thread for it with a 16 MB stack: the lifted code nests a host call for every guest
 *     call. (hbloader's own main thread is smaller.)
 *
 * And with -Wl,--wrap=pthread_attr_setstacksize, host stacks above 16 MB get 16 MB. The runtime
 * asks 256 MB for each PPU thread, a reservation the PC's kernel fills only as it is used;
 * libnx takes a thread's whole stack from the heap at once.
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <switch.h>

#include "ps3emu/ppu_lift.h"

#include "../hos/hos_guest.h"
#include "nx_crash.h"
#include "nx_data.h"
#include "nx_game_sdl.h"
#include "nx_game_threads.h"
#include "nx_guest.h"
#include "nx_log.h"
#include "nx_paths.h"
#include "nx_settings.h"
#include "nx_system.h"
#include "nx_vfs.h"

#define HOST_STACK_MAX  (16u << 20)
#define BOOT_STACK      HOST_STACK_MAX
#define EBOOT_PATH      NX_GAME_DIR "/PS3_GAME/USRDIR/EBOOT.elf"
#define LIBSRE_IMAGE    NX_FIRMWARE_DIR "/libsre_image.bin"

/* the RSX's local memory, 256 MB: always there on a PS3, and the game fills it with its
 * textures and frame buffers, so it is one mapping from the start, not hundreds of faults */
#define RSX_LOCAL_BASE  0xC0000000ull
#define RSX_LOCAL_SIZE  0x10000000ull

extern uint8_t *vm_base;                            /* boot_main.cpp */
extern __thread ppu_context *g_active_ctx;          /* the runtime's: the PPU code this thread runs */
unsigned nx_port_flips(void);                       /* nx_port_glue.c: the game's frames so far */
extern unsigned char g_vm_page_bitmap[65536 / 8];   /* runtime/spu/spu_vm_pagemap.c */

int __real_main(int argc, char **argv);
int __real_pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);
int __real_setvbuf(FILE *stream, char *buf, int mode, size_t size);

/* With -Wl,--wrap=setvbuf. The runtime makes stdout unbuffered as the PPU starts
 * (ppu_loader.cpp's ppu_run), so that no line waits in a buffer when a run is killed. Here
 * stdout is the log's own stream, and newlib writes a printf to an unbuffered stream outside
 * the stream's lock: two threads at once wrote at the same offset, and moved it past a gap the
 * SD card fills with whatever its sectors held. Line buffering keeps the lock and still writes
 * each line as it ends, which is what the runtime wants. */
int __wrap_setvbuf(FILE *stream, char *buf, int mode, size_t size)
{
    if (stream && stream == nx_log_stream() && mode == _IONBF)
        return 0;   /* it stays line-buffered (nx_log.c) */
    return __real_setvbuf(stream, buf, mode, size);
}

static unsigned s_threads, s_capped;
static unsigned long long s_stack_bytes;

int __wrap_pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)
{
    if (size > HOST_STACK_MAX) {
        if (__atomic_fetch_add(&s_capped, 1, __ATOMIC_RELAXED) == 0)
            nx_log("threads: a host stack of %zu MB asked for; above %u MB, %u MB is given\n", size >> 20,
                   HOST_STACK_MAX >> 20, HOST_STACK_MAX >> 20);
        size = HOST_STACK_MAX;
    }
    const int rc = __real_pthread_attr_setstacksize(attr, size);
    if (rc == 0) {
        __atomic_fetch_add(&s_threads, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s_stack_bytes, (unsigned long long)size, __ATOMIC_RELAXED);
    }
    return rc;
}

/* ---- crashes (nx_crash.c) ---------------------------------------------------------------- */

/* For the crash report, on the faulting thread: the PPU code it was running, if any. */
static size_t guest_context(char *out, size_t size)
{
    const ppu_context *c = g_active_ctx;
    if (!c)
        return (size_t)snprintf(out, size, "the PS3 code: none on this thread\n");
    if (!nx_crash_readable((uint64_t)(uintptr_t)c, sizeof *c))
        return (size_t)snprintf(out, size, "the PS3 code: its context at %p cannot be read\n", (const void *)c);
    size_t used = 0;
    int n = snprintf(out, size, "the PS3 code (PPU thread %llu): cia 0x%08llx, lr 0x%08llx, ctr 0x%08llx\n",
                     (unsigned long long)c->thread_id, (unsigned long long)c->cia, (unsigned long long)c->lr,
                     (unsigned long long)c->ctr);
    for (int r = 0; n > 0 && (used += (size_t)n) < size && r < 32; r += 4)
        n = snprintf(out + used, size - used, "  r%-2d 0x%016llx  r%-2d 0x%016llx  r%-2d 0x%016llx  r%-2d 0x%016llx\n",
                     r, (unsigned long long)c->gpr[r], r + 1, (unsigned long long)c->gpr[r + 1], r + 2,
                     (unsigned long long)c->gpr[r + 2], r + 3, (unsigned long long)c->gpr[r + 3]);
    return used < size ? used : size - 1;
}

/* With -Wl,--wrap=abort and --wrap=__assert_func: a report from where they were called, then
 * what they do anyway. */
void __real_abort(void) __attribute__((noreturn));
void __real___assert_func(const char *file, int line, const char *func, const char *expr) __attribute__((noreturn));

void __wrap_abort(void)
{
    nx_crash_report_here("abort()");
    __real_abort();
}

void __wrap___assert_func(const char *file, int line, const char *func, const char *expr)
{
    char why[512];
    snprintf(why, sizeof why, "assert(%s) failed, in %s, %s:%d", expr ? expr : "?", func ? func : "?",
             file ? file : "?", line);
    nx_crash_report_here(why);
    __real___assert_func(file, line, func, expr);
}

/* Something the game needs is not there: say so on the screen, and wait for + to exit. */
static int stop(const char *what, const char *detail)
{
    nx_log("game: cannot start: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    printf("\x1b[1;1HDemon's Souls for Nintendo Switch %s\n", DESNX_VERSION);
    printf("-----------------------------------------\n\n");
    printf("The game cannot start:\n\n  %s\n", what);
    if (detail)
        printf("  %s\n", detail);
    printf("\nThe log is in " NX_LOG_DIR "/" NX_LOG_FILE "\n\nPress + to exit.\n");
    consoleUpdate(NULL);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        svcSleepThread(16 * 1000 * 1000);
    }
    consoleExit(NULL);
    return 1;
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void log_guest_memory(const char *when)
{
    NxGuestStats stats;
    nx_guest_stats(&stats);
    /* one write, so that no other thread's line lands inside it */
    char refused[64] = "";
    if (stats.refused)
        snprintf(refused, sizeof refused, " (the first at %#llx, result %#x)", (unsigned long long)stats.refused_at,
                 stats.refused_rc);
    nx_log("guest memory %s: %llu MB in %u mappings, %u faults answered, %u refused%s; host stacks so far: %u, "
           "%llu MB, %u capped\n", when, (unsigned long long)(stats.mapped >> 20), stats.maps, stats.faults,
           stats.refused, refused, s_threads, s_stack_bytes >> 20, s_capped);
}

/* The log stays open: stdout and stderr are its stream, and other threads may still print.
 * The C library's exit flushes and closes it after this. */
static void at_exit(void)
{
    log_guest_memory("at the exit");
    nx_guest_close();
    fflush(stdout);
}

/* Every 10 seconds: the sound, and guest memory while it changes. A game that hangs or crashes
 * never reaches the exit. */
static void *watch_thread(void *arg)
{
    (void)arg;
    NxGuestStats last = {0};
    unsigned last_threads = 0;
    NxThreadStats last_detached = {0};
    unsigned last_flips = 0;
    /* DESNX_CRASH_TEST=<seconds> (settings.txt): a write to address 8 then, on purpose, to try the
     * crash report */
    const char *crash_test = getenv("DESNX_CRASH_TEST");
    const unsigned crash_at = crash_test ? (unsigned)atoi(crash_test) : 0;
    for (unsigned seconds = 10;; seconds += 10) {
        svcSleepThread(10ull * 1000 * 1000 * 1000);
        if (crash_at && seconds >= crash_at) {
            nx_log("crash test: a write to address 8, on purpose (DESNX_CRASH_TEST=%u)\n", crash_at);
            fflush(stdout);
            volatile uint32_t *volatile nowhere = (volatile uint32_t *)(uintptr_t)8;
            *nowhere = 1;
        }
        const unsigned flips = nx_port_flips();
        nx_log("frames: %u in the last 10 s, %u in all\n", flips - last_flips, flips);
        last_flips = flips;
        nx_game_sdl_report();
        NxThreadStats detached;
        nx_game_threads_stats(&detached);
        if (detached.freed != last_detached.freed || detached.detached != last_detached.detached ||
            detached.lost != last_detached.lost)
            nx_log("threads: %u detached ones ended and given back, %u not ended yet (at most %u at once)%s\n",
                   detached.freed, detached.detached, detached.peak,
                   detached.lost ? ", and some the list had no room for" : "");
        last_detached = detached;
        NxGuestStats now;
        nx_guest_stats(&now);
        if (!now.window)
            return NULL;
        if (now.maps != last.maps || now.faults != last.faults || now.refused != last.refused ||
            s_threads != last_threads) {
            char when[32];
            snprintf(when, sizeof when, "after %u s", seconds);
            log_guest_memory(when);
        }
        last = now;
        last_threads = s_threads;
    }
}

static int s_argc, s_rc;
static char **s_argv;

static void *boot_thread(void *arg)
{
    (void)arg;
    s_rc = __real_main(s_argc, s_argv);
    return NULL;
}

int __wrap_main(int argc, char **argv)
{
    nx_log_init(DESNX_VERSION ", the game");
    nx_crash_init(DESNX_VERSION ", the game", guest_context);
    nx_vfs_init();
    nx_settings_load();     /* after the drives, so a setting may name another */
    nx_settings_args(argc, argv);
    NxSystem system;
    nx_system_read(&system);
    nx_system_log(&system);
    if (!system.application)
        return stop("It was started as an applet, which gets too little memory.",
                    "Hold R while starting a game, then pick it from the homebrew menu.");

    NxData data;
    nx_data_check(&data);
    nx_data_log(&data);
    if (!data.found)
        return stop("The game's folder was not found.", NX_GAME_DIR);
    if (!data.right_version)
        return stop("The game's folder holds another game or version: BLUS30443 01.00 is needed.", NX_GAME_DIR);
    if (data.first_missing)
        return stop("A folder of the game is missing from USRDIR.", data.first_missing);
    if (!exists(EBOOT_PATH))
        return stop("EBOOT.elf, decrypted on the PC with your RPCS3, is not on the SD card.", EBOOT_PATH);
    if (!exists(LIBSRE_IMAGE))
        return stop("libsre_image.bin, made on the PC from your firmware's libsre.prx, is not on the SD card.",
                    LIBSRE_IMAGE);

    uint8_t *window = nx_guest_open(g_vm_page_bitmap);
    if (!window)
        return stop("The PS3's memory could not be set up: see the log.", NULL);
    hos_guest_window(window, NX_GUEST_SIZE, nx_guest_commit);
    vm_base = window;
    atexit(at_exit);
    const uint32_t rc = nx_guest_commit(RSX_LOCAL_BASE, RSX_LOCAL_SIZE);
    if (rc)
        return stop("The RSX's 256 MB could not be mapped: see the log.", NULL);
    log_guest_memory("at the start");

    nx_log_capture_stdout();
    static char *args[3];
    args[0] = argc > 0 && argv[0] ? argv[0] : "demonsouls_game.nro";
    args[1] = EBOOT_PATH;
    s_argc = 2;
    s_argv = args;

    pthread_t watch;
    if (pthread_create(&watch, NULL, watch_thread, NULL) == 0)
        pthread_detach(watch);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, BOOT_STACK);
    pthread_t boot;
    int err = pthread_create(&boot, &attr, boot_thread, NULL);
    pthread_attr_destroy(&attr);
    if (err) {
        nx_log("game: the boot's thread could not start (%d)\n", err);
        return 1;
    }
    pthread_join(boot, NULL);
    nx_log("game: the boot returned %d\n", s_rc);
    return s_rc;
}
