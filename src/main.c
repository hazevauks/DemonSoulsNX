/* main.c -- the test shell, until the game runs.
 *
 * This NRO has no game code yet. It checks what the later stages depend on, on the console:
 * stage 1, that the NRO starts, that logs reach SD_LOG, how much memory the process gets, and
 * that the PS3's 4 GB address space maps in it (nx_guest.c); stage 2, that the game's files
 * open by the PS3's paths, inflate, read and write at a usable speed, and which of the files
 * decrypted on the PC are there (nx_vfs_probe.c). The recompiled game takes over from here
 * in a later stage (docs/status.md).
 */

#include <stdio.h>

#include <switch.h>

#include "platform/nx_data.h"
#include "platform/nx_guest.h"
#include "platform/nx_log.h"
#include "platform/nx_paths.h"
#include "platform/nx_system.h"
#include "platform/nx_vfs.h"

static void show(const NxSystem *system, const NxGuestProbe *guest, const NxData *data, const NxVfsProbe *files)
{
    printf("\x1b[1;1H\x1b[2J");
    printf("Demon's Souls for Nintendo Switch %s\n", DESNX_VERSION);
    printf("-----------------------------------------\n\n");
    printf("This build checks the console and the game's files.\n");
    printf("The game itself is not in this build yet.\n\n");

    printf("Launch mode:  %s\n", system->application ? "application (full memory)" : "applet: start it through a forwarder or title takeover");
    printf("Memory:       %lu MB for the game\n", (unsigned long)(system->memory_total >> 20));
    if (!guest->failed)
        printf("Guest memory: works (%d-bit space, %u faults answered)\n\n", guest->address_bits, guest->faults);
    else
        printf("Guest memory: failed at %s,\n              see the log\n\n", guest->failed);

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
        printf("Decrypted:    EBOOT.elf %s, scripts %u of %u, libsre.prx %s\n", files->eboot ? "yes" : "no",
               files->scripts, files->scripts_expected, files->libsre ? "yes" : "no");
    }

    printf("\nThe log is in " NX_LOG_DIR "/" NX_LOG_FILE "\n\n");
    printf("Press + to exit.\n");
    consoleUpdate(NULL);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    nx_log_init(DESNX_VERSION);
    nx_vfs_init();
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    NxSystem system;
    nx_system_read(&system);
    nx_system_log(&system);

    NxGuestProbe guest;
    nx_guest_probe(&guest);

    NxData data;
    nx_data_check(&data);
    nx_data_log(&data);

    printf("Checking the game's files...\n");
    consoleUpdate(NULL);
    NxVfsProbe files = {0};
    if (data.found && data.right_version)
        nx_vfs_probe(&files);

    show(&system, &guest, &data, &files);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        svcSleepThread(16 * 1000 * 1000);
    }

    nx_log("exit\n");
    consoleExit(NULL);
    nx_log_shutdown();
    return 0;
}
