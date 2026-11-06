/* main.c -- the test shell, until the game runs.
 *
 * This NRO has no game code yet. It checks what the later stages depend on, on the console:
 * stage 1, that the NRO starts, that logs reach SD_LOG, how much memory the process gets, and
 * that the PS3's 4 GB address space maps in it (nx_guest.c); stage 2, that the game's files
 * open by the PS3's paths, inflate, read and write at a usable speed, and which of the files
 * decrypted on the PC are there (nx_vfs_probe.c); stage 3, that deko3D draws with shaders uam
 * built (nx_gpu_probe.c), and what the cores give the game's threads (nx_cpu_probe.c); stage 5,
 * that SDL2's game controllers and audio, which the runtime's pad and sound modules use, work in
 * the same NRO as deko3D (nx_sdl_probe.c). The recompiled game takes over from here in a later
 * stage (docs/status.md).
 */

#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "platform/nx_cpu.h"
#include "platform/nx_data.h"
#include "platform/nx_gpu.h"
#include "platform/nx_guest.h"
#include "platform/nx_log.h"
#include "platform/nx_paths.h"
#include "platform/nx_sdl.h"
#include "platform/nx_system.h"
#include "platform/nx_vfs.h"

static void show(const NxSystem *system, const NxGuestProbe *guest, const NxGpuProbe *gpu, const NxCpuProbe *cpu,
                 const NxSdlProbe *sdl, const NxData *data, const NxVfsProbe *files)
{
    printf("\x1b[1;1H\x1b[2J");
    printf("Demon's Souls for Nintendo Switch %s\n", DESNX_VERSION);
    printf("-----------------------------------------\n\n");
    printf("This build checks the console and the game's files.\n");
    printf("The game itself is not in this build yet.\n\n");

    printf("Launch mode:  %s\n", system->application ? "application (full memory)" : "applet: start it through a forwarder or title takeover");
    printf("Memory:       %lu MB for the game\n", (unsigned long)(system->memory_total >> 20));
    if (!guest->failed)
        printf("Guest memory: works (%d-bit space, %u faults answered)\n", guest->address_bits, guest->faults);
    else
        printf("Guest memory: failed at %s,\n              see the log\n", guest->failed);
    if (!gpu->failed)
        printf("GPU:          deko3D works, %u frames at %u.%u ms\n", gpu->frames, gpu->frame_us / 1000,
               gpu->frame_us / 100 % 10);
    else
        printf("GPU:          failed at %s\n", gpu->failed);
    printf("Cores:        %u for the game, a wake takes %u us\n", cpu->cores, (cpu->wake_cross_ns + 500) / 1000);
    if (sdl->failed) {
        printf("SDL2:         %s failed at %s, see the log\n\n", sdl->version, sdl->failed);
    } else {
        printf("Controls:     SDL2 %s, %d controller%s%s%s\n", sdl->version, sdl->controllers,
               sdl->controllers == 1 ? "" : "s", sdl->controller[0] ? ": " : "", sdl->controller);
        printf("Sound:        %d Hz, %d channels; frames played a second, by buffer size:\n             ",
               sdl->audio_freq, sdl->audio_channels);
        for (int i = 0; i < NX_SDL_AUDIO_SIZES; i++)
            printf(" %d: %u ", sdl->audio_sizes[i], sdl->audio_rates[i]);
        printf("\n\n");
    }

    if (!data->found)
        printf("Game files:   not found. Copy the game's folder to\n              " NX_GAME_DIR "/\n");
    else if (!data->right_version)
        printf("Game files:   %s %s, but the port needs BLUS30443 01.00\n", data->title_id, data->version);
    else if (data->first_missing)
        printf("Game files:   USRDIR/%s is missing (%d of %d folders)\n", data->first_missing,
               data->folders_found, data->folders_expected);
    else
        printf("Game files:   %s %s, all %d folders found\n", data->title_id, data->version, data->folders_expected);

    if (data->found && data->right_version) {
        if (!files->failed)
            printf("File layer:   works, %u MB/s, %u us a small file\n", files->read_mbps, files->small_file_us);
        else
            printf("File layer:   failed at %s, see the log\n", files->failed);
        printf("Decrypted:    EBOOT.elf %s, scripts %u of %u, libsre image %s\n", files->eboot ? "yes" : "no",
               files->scripts, files->scripts_expected, files->libsre ? "yes" : "no");
    }

    printf("\nThe log is in " NX_LOG_DIR "/" NX_LOG_FILE "\n\n");
    printf("Press each button once: the log notes how SDL2 names it.\n");
    printf("Press + to exit.\n\n");
    printf("Buttons:      \x1b[s");      /* nx_sdl_pad_line's place, kept for the loop */
    consoleUpdate(NULL);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    nx_log_init(DESNX_VERSION);
    nx_vfs_init();
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    /* SDL2's controllers and sound before deko3D, so deko3D starts with both running, as in
     * the game; SDL2's two tones play, after about two seconds of silence, just before the
     * triangle */
    NxSdlProbe sdl;
    nx_sdl_probe(&sdl);

    /* deko3D takes the window for two seconds, then gives it to the text console */
    NxGpuProbe gpu;
    nx_gpu_probe(&gpu);
    consoleInit(NULL);

    NxSystem system;
    nx_system_read(&system);
    nx_system_log(&system);

    NxGuestProbe guest;
    nx_guest_probe(&guest);

    NxCpuProbe cpu;
    nx_cpu_probe(&cpu);

    NxData data;
    nx_data_check(&data);
    nx_data_log(&data);

    printf("Checking the game's files...\n");
    consoleUpdate(NULL);
    NxVfsProbe files = {0};
    if (data.found && data.right_version)
        nx_vfs_probe(&files);

    show(&system, &guest, &gpu, &cpu, &sdl, &data, &files);
    char line[128], shown[128] = "";
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        if (!sdl.failed) {
            nx_sdl_pad_line(line, 66);  /* what fits after "Buttons:" on the 80 columns */
            if (strcmp(line, shown) != 0) {
                printf("\x1b[u\x1b[K%s", line);
                consoleUpdate(NULL);
                strcpy(shown, line);
            }
        }
        svcSleepThread(16 * 1000 * 1000);
    }

    nx_log("exit\n");
    nx_sdl_shutdown();
    consoleExit(NULL);
    nx_log_shutdown();
    return 0;
}
