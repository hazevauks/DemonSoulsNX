/* nx_vfs.c -- the PS3's drives on the SD card.
 *
 * The game names its files by the PS3's drives: the disc (/dev_bdvd, or /app_home for the
 * game's own folder), the internal drive (/dev_hdd0: saves) and the game's cache partition
 * (/dev_hdd1). ps3recomp's file layer (runtime/ppu/ppu_fs.cpp) maps each drive onto a host
 * folder it reads from the environment. This sets those to folders on the SD card, and maps
 * paths the same way for the port's own code and tests.
 *
 * The SD card's FAT32 or exFAT ignores case, as the PS3's disc does, which the game relies
 * on: other/ingamestay.loadlist asks for "other/envLight.tpf", and the disc has envlight.tpf.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ps3emu/vfs_path.h"    /* ps3recomp's own checks on a game path */

#include "nx_log.h"
#include "nx_paths.h"
#include "nx_vfs.h"

static const struct {
    const char *drive;
    const char *folder;
} drives[] = {
    {"/dev_bdvd", NX_GAME_DIR},
    {"/app_home", NX_GAME_DIR},
    {"/dev_hdd0", NX_HDD0_DIR},
    {"/dev_hdd1", NX_HDD1_DIR},
};

/* drives the PS3 has and the port does not: its firmware, USB sticks, the PC */
static const char *const absent[] = {"/dev_flash", "/dev_usb000", "/dev_usb", "/host_root"};

void nx_vfs_init(void)
{
    mkdir(NX_HDD0_DIR, 0777);
    mkdir(NX_HDD1_DIR, 0777);
    mkdir(NX_FIRMWARE_DIR, 0777);
    setenv("PS3_VFS_ROOT", NX_GAME_DIR, 1);
    setenv("PS3_HDD0_ROOT", NX_HDD0_DIR, 1);
    setenv("PS3_HDD1_ROOT", NX_HDD1_DIR, 1);
    /* the image tools/make_sd.py copies from the lift of libsre (work/libsre/libsre_image.bin) */
    setenv("PS3_LIBSRE_IMAGE", NX_FIRMWARE_DIR "/libsre_image.bin", 1);
}

/* path is `drive` or below it; *rest is what follows the drive's '/'. */
static bool on_drive(const char *path, const char *drive, const char **rest)
{
    size_t n = strlen(drive);
    if (strncmp(path, drive, n) != 0 || (path[n] != '/' && path[n] != '\0'))
        return false;
    *rest = path + n + (path[n] == '/');
    return true;
}

bool nx_vfs_host_path(const char *guest, char *out, size_t cap)
{
    char clean[1024];
    if (!ps3_vfs_guest_path(guest, clean, sizeof clean, "nx_vfs"))
        return false;

    const char *rest;
    for (size_t i = 0; i < sizeof absent / sizeof absent[0]; i++)
        if (on_drive(clean, absent[i], &rest))
            return false;

    /* a path with no drive is in the game's folder, as ppu_fs.cpp has it */
    const char *folder = NX_GAME_DIR;
    rest = clean[0] == '/' ? clean + 1 : clean;
    for (size_t i = 0; i < sizeof drives / sizeof drives[0]; i++)
        if (on_drive(clean, drives[i].drive, &rest)) {
            folder = drives[i].folder;
            break;
        }

    int n = snprintf(out, cap, "%s%s%s", folder, *rest ? "/" : "", rest);
    return n > 0 && (size_t)n < cap;
}
