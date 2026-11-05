/* main.c -- the first boot.
 *
 * Stage 1 of the port: this NRO has no game code yet. It checks what the later stages
 * depend on: that the NRO starts, that logs reach SD_LOG, that the game's folder is on the
 * SD card, and how much memory and address space the console gives the process. The
 * recompiled game takes over from here in a later stage (docs/status.md).
 */

#include <stdio.h>

#include <switch.h>

#include "platform/nx_data.h"
#include "platform/nx_log.h"
#include "platform/nx_paths.h"
#include "platform/nx_system.h"

static void show(const NxSystem *system, const NxData *data)
{
    printf("\x1b[1;1H\x1b[2J");
    printf("Demon's Souls for Nintendo Switch %s\n", DESNX_VERSION);
    printf("-----------------------------------------\n\n");
    printf("This build checks the console and the game's files.\n");
    printf("The game itself is not in this build yet.\n\n");

    printf("Launch mode:  %s\n", system->application ? "application (full memory)" : "applet: start it through a forwarder or title takeover");
    printf("Memory:       %lu MB for the game\n", (unsigned long)(system->memory_total >> 20));
    printf("Guest memory: %s\n\n", system->map_physical_memory ? "can be mapped" : "cannot be mapped in this mode");

    if (!data->found)
        printf("Game files:   not found. Copy the game's folder to\n              " NX_GAME_DIR "/\n");
    else if (!data->right_version)
        printf("Game files:   %s %s, but the port needs BLUS30443 01.00\n", data->title_id, data->version);
    else if (data->first_missing)
        printf("Game files:   USRDIR/%s is missing (%d of %d folders)\n", data->first_missing,
               data->folders_found, data->folders_expected);
    else
        printf("Game files:   %s %s, all %d folders found\n", data->title_id, data->version, data->folders_expected);

    printf("\nThe log is in " NX_LOG_DIR "/" NX_LOG_FILE "\n\n");
    printf("Press + to exit.\n");
    consoleUpdate(NULL);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    nx_log_init(DESNX_VERSION);
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    NxSystem system;
    nx_system_read(&system);
    nx_system_log(&system);

    NxData data;
    nx_data_check(&data);
    nx_data_log(&data);

    show(&system, &data);
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
