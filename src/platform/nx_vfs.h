/* nx_vfs.h -- the PS3's drives on the SD card. */

#ifndef NX_VFS_H
#define NX_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the folders behind the PS3's drives and tells ps3recomp's file layer where they are:
 * PS3_VFS_ROOT (/dev_bdvd and /app_home), PS3_HDD0_ROOT, PS3_HDD1_ROOT, and PS3_LIBSRE_IMAGE
 * (the lifted libsre's image, for its glue). */
void nx_vfs_init(void);

/* A path as the game asks for it ("/dev_bdvd/PS3_GAME/USRDIR/...") as a path on the SD card,
 * mapped the way ps3recomp's file layer maps it with the folders above. False for a path that
 * leaves the game's drives, or a drive the port does not have (/dev_flash, /dev_usb). */
bool nx_vfs_host_path(const char *guest, char *out, size_t cap);

typedef struct {
    unsigned paths_ok, paths;       /* game paths that open and hold what they should */
    bool refused_ok;                /* paths out of the drives are refused */
    bool ignores_case;              /* "envLight.tpf", as the game writes it, opens envlight.tpf */
    bool inflate_ok;                /* DCX files inflate on the console */
    uint32_t inflate_mbps;
    uint32_t read_mbps;             /* one big file, 1 MB at a time */
    uint32_t seek_us;               /* a 64 KB read at a random place */
    uint32_t small_file_us;         /* open, read 4 KB, close */
    bool write_ok;                  /* the cache's way of writing: set the size, write, read back */
    uint32_t write_mbps;
    uint64_t free_mb;               /* free space on the SD card */
    bool eboot;                     /* PS3_GAME/USRDIR/EBOOT.elf, decrypted */
    unsigned scripts, scripts_expected;  /* .sdat.unedat copies beside their .sdat */
    bool libsre;                    /* firmware/libsre_image.bin, the lifted libsre's image */
    const char *failed;             /* the first check that failed, or NULL */
} NxVfsProbe;

/* Stage 2: opens, reads, inflates and writes game files the way the game will. */
void nx_vfs_probe(NxVfsProbe *out);

#ifdef __cplusplus
}
#endif

#endif /* NX_VFS_H */
