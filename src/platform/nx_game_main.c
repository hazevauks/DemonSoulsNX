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

#include "../hos/hos_guest.h"
#include "nx_data.h"
#include "nx_guest.h"
#include "nx_log.h"
#include "nx_paths.h"
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
extern unsigned char g_vm_page_bitmap[65536 / 8];   /* runtime/spu/spu_vm_pagemap.c */

int __real_main(int argc, char **argv);
int __real_pthread_attr_setstacksize(pthread_attr_t *attr, size_t size);

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
    nx_log("guest memory %s: %llu MB in %u mappings, %u faults answered, %u refused", when,
           (unsigned long long)(stats.mapped >> 20), stats.maps, stats.faults, stats.refused);
    if (stats.refused)
        nx_log(" (the first at %#llx, result %#x)", (unsigned long long)stats.refused_at, stats.refused_rc);
    nx_log("; host stacks: %u threads, %llu MB, %u capped\n", s_threads, s_stack_bytes >> 20, s_capped);
}

static void at_exit(void)
{
    log_guest_memory("at the exit");
    nx_guest_close();
    nx_log_shutdown();
}

/* Every 10 seconds, while it changes: a game that hangs or crashes never reaches the exit. */
static void *watch_thread(void *arg)
{
    (void)arg;
    NxGuestStats last = {0};
    unsigned last_threads = 0;
    for (unsigned seconds = 10;; seconds += 10) {
        svcSleepThread(10ull * 1000 * 1000 * 1000);
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
    nx_vfs_init();
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
