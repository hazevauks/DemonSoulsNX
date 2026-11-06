/* nx_vfs_probe.c -- stage 2: the game's files on the console, read the way the game reads them.
 *
 * Opens files by the paths the game uses at its start (Soulbrandt's boot logs, its docs/23 and
 * docs/25), in the case the game writes them; inflates DCX files as the game's decompressor
 * does (tools/check_archives.py has the layout); times the SD card for the three ways the game
 * reads (long streams, seeks, many small files) and for the way it writes its cache; and looks
 * for the files decrypted on the PC. What it writes goes in /dev_hdd1 and is removed again.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <switch.h>
#include <zlib.h>

#include "nx_log.h"
#include "nx_paths.h"
#include "nx_vfs.h"

#define USRDIR "/dev_bdvd/PS3_GAME/USRDIR/"
#define MB (1024 * 1024)

#define EBOOT_ELF_SIZE  28222408u       /* third_party/soulbrandt/tools/setup/des_setup/game_table.json */

static uint64_t us_since(uint64_t tick)
{
    return armTicksToNs(armGetSystemTick() - tick) / 1000;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static uint32_t rate(uint64_t bytes, uint64_t us)
{
    return us ? (uint32_t)(bytes * 1000000 / MB / us) : 0;
}

/* The whole file, or NULL. */
static uint8_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    struct stat st;
    uint8_t *data = NULL;
    if (fstat(fileno(f), &st) == 0 && st.st_size > 0 && (data = malloc(st.st_size)) != NULL) {
        if (fread(data, 1, st.st_size, f) == (size_t)st.st_size) {
            *size = st.st_size;
        } else {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    return data;
}

/* The file's size and first four bytes; false if it does not open. */
static bool peek(const char *path, uint8_t head[4], uint64_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    struct stat st;
    bool ok = fstat(fileno(f), &st) == 0 && fread(head, 1, 4, f) == 4;
    *size = ok ? (uint64_t)st.st_size : 0;
    fclose(f);
    return ok;
}

/* ---- the paths the game opens first -------------------------------------------------- */

static const struct {
    const char *guest;
    char magic[5];
} boot_files[] = {
    {"/dev_bdvd/PS3_GAME/PARAM.SFO", "\0PSF"},
    {USRDIR "shader/ds_flver.shaderbnd.dcx", "DCX\0"},
    {USRDIR "menu/mapshot.tpf.dcx", "DCX\0"},
    {USRDIR "other/default.rumblebnd", "BND3"},
    {USRDIR "other/envLight.tpf", "TPF\0"},             /* the loadlist's case; the disc has envlight.tpf */
    {"/app_home/PS3_GAME/USRDIR/shader/ds_sfx.shaderbnd.dcx", "DCX\0"},
};

static const char *const refused_paths[] = {
    "/dev_bdvd/../../atmosphere/config",                /* out of the drive */
    "/dev_flash/sys/external/libsre.sprx",              /* a drive the port does not have */
};

static void check_paths(NxVfsProbe *out)
{
    char host[512];
    out->paths = sizeof boot_files / sizeof boot_files[0];
    for (unsigned i = 0; i < out->paths; i++) {
        uint8_t head[4];
        uint64_t size;
        bool ok = nx_vfs_host_path(boot_files[i].guest, host, sizeof host) && peek(host, head, &size) &&
                  memcmp(head, boot_files[i].magic, 4) == 0;
        out->paths_ok += ok;
        if (strstr(boot_files[i].guest, "envLight"))
            out->ignores_case = ok;
        nx_log("files: %s %s\n", boot_files[i].guest, ok ? "opens" : "does NOT open or holds something else");
    }

    out->refused_ok = true;
    for (unsigned i = 0; i < sizeof refused_paths / sizeof refused_paths[0]; i++)
        if (nx_vfs_host_path(refused_paths[i], host, sizeof host)) {
            out->refused_ok = false;
            nx_log("files: %s was NOT refused (%s)\n", refused_paths[i], host);
        }
}

/* ---- DCX, as the game's decompressor reads it ------------------------------------------ */

/* Inflates a DCX file's EDGE blocks into out[out_size]. */
static bool dcx_inflate(const uint8_t *d, size_t size, uint8_t *out, size_t out_size)
{
    if (size < 0x70 || memcmp(d, "DCX\0", 4) != 0 || memcmp(d + 0x24, "DCP\0EDGE", 8) != 0 ||
        memcmp(d + 0x44, "DCA\0", 4) != 0 || memcmp(d + 0x4C, "EgdT", 4) != 0 || be32(d + 0x1C) != out_size)
        return false;
    const uint32_t last = be32(d + 0x60), count = be32(d + 0x68);
    const size_t base = 0x44 + be32(d + 0x48);
    if (0x70 + (size_t)count * 16 > size)
        return false;

    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK)
        return false;
    size_t done = 0;
    bool ok = true;
    for (uint32_t i = 0; ok && i < count; i++) {
        const uint8_t *entry = d + 0x70 + i * 16;
        const uint32_t offset = be32(entry + 4), length = be32(entry + 8), packed = be32(entry + 12);
        const uint32_t want = i + 1 == count ? last : 0x10000;
        if (base + offset + length > size || done + want > out_size) {
            ok = false;
        } else if (packed) {
            inflateReset(&z);
            z.next_in = (Bytef *)(d + base + offset);
            z.avail_in = length;
            z.next_out = out + done;
            z.avail_out = want;
            ok = inflate(&z, Z_FINISH) == Z_STREAM_END && z.avail_out == 0;
        } else {
            ok = length == want;
            if (ok)
                memcpy(out + done, d + base + offset, length);
        }
        done += want;
    }
    inflateEnd(&z);
    return ok && done == out_size;
}

static const struct {
    const char *guest;
    char inside[5];
} packed_files[] = {
    {USRDIR "shader/ds_flver.shaderbnd.dcx", "BND3"},
    {USRDIR "menu/00.tpf.dcx", "TPF\0"},
    {USRDIR "chr/c0000/c0000.anibnd.dcx", "BND3"},
};

static void check_inflate(NxVfsProbe *out)
{
    uint64_t inflated = 0, us = 0;
    out->inflate_ok = true;
    for (unsigned i = 0; i < sizeof packed_files / sizeof packed_files[0]; i++) {
        char host[512];
        size_t size = 0;
        uint8_t *data = nx_vfs_host_path(packed_files[i].guest, host, sizeof host) ? load(host, &size) : NULL;
        const size_t out_size = data && size >= 0x20 ? be32(data + 0x1C) : 0;
        uint8_t *plain = out_size ? malloc(out_size) : NULL;
        const uint64_t start = armGetSystemTick();
        bool ok = plain && dcx_inflate(data, size, plain, out_size) && memcmp(plain, packed_files[i].inside, 4) == 0;
        us += us_since(start);
        inflated += ok ? out_size : 0;
        out->inflate_ok &= ok;
        nx_log("files: %s %s (%u KB to %u KB)\n", packed_files[i].guest, ok ? "inflates" : "does NOT inflate",
               (unsigned)(size >> 10), (unsigned)(out_size >> 10));
        free(plain);
        free(data);
    }
    out->inflate_mbps = rate(inflated, us);
    nx_log("files: inflating at %u MB/s on one core\n", out->inflate_mbps);
}

/* ---- the SD card's speed --------------------------------------------------------------- */

static void check_reads(NxVfsProbe *out)
{
    char host[512];
    uint8_t *buffer = malloc(MB);
    if (!buffer)
        return;

    /* a long stream: 64 MB of the opening movie, 1 MB at a time */
    FILE *f = nx_vfs_host_path(USRDIR "movie/ds_opening.pam", host, sizeof host) ? fopen(host, "rb") : NULL;
    if (f) {
        uint64_t got = 0, start = armGetSystemTick();
        for (int i = 0; i < 64; i++)
            got += fread(buffer, 1, MB, f);
        out->read_mbps = rate(got, us_since(start));
        fclose(f);
    }

    /* seeks: 128 reads of 64 KB at scattered places in the other movie */
    f = nx_vfs_host_path(USRDIR "movie/ds_advertise.pam", host, sizeof host) ? fopen(host, "rb") : NULL;
    if (f) {
        struct stat st;
        fstat(fileno(f), &st);
        const uint64_t blocks = (uint64_t)st.st_size / 0x10000;
        uint32_t seed = 12345;
        uint64_t start = armGetSystemTick();
        for (int i = 0; i < 128 && blocks; i++) {
            seed = seed * 1103515245u + 12345u;
            fseeko(f, (off_t)((seed >> 8) % blocks) * 0x10000, SEEK_SET);
            fread(buffer, 1, 0x10000, f);
        }
        out->seek_us = (uint32_t)(us_since(start) / 128);
        fclose(f);
    }

    /* many small files: open, read 4 KB, close, for each file of one level's folder */
    DIR *dir = nx_vfs_host_path(USRDIR "map/m01_00_00_00", host, sizeof host) ? opendir(host) : NULL;
    if (dir) {
        unsigned files = 0;
        uint64_t start = armGetSystemTick();
        for (struct dirent *e; (e = readdir(dir)) != NULL;) {
            char path[768];
            snprintf(path, sizeof path, "%s/%s", host, e->d_name);
            if ((f = fopen(path, "rb")) != NULL) {
                fread(buffer, 1, 4096, f);
                fclose(f);
                files++;
            }
        }
        closedir(dir);
        out->small_file_us = files ? (uint32_t)(us_since(start) / files) : 0;
        nx_log("files: %u files of map/m01_00_00_00 opened, read and closed\n", files);
    }
    free(buffer);
    nx_log("files: reading %u MB/s in 1 MB pieces, %u us for a 64 KB read elsewhere, %u us for a small file\n",
           out->read_mbps, out->seek_us, out->small_file_us);
}

/* The game writes a cache copy by setting its full size first, then writing it in pieces. */
static void check_writes(NxVfsProbe *out)
{
    char path[512], moved[512];
    if (!nx_vfs_host_path("/dev_hdd1/nx_probe.bin", path, sizeof path) ||
        !nx_vfs_host_path("/dev_hdd1/nx_probe_moved.bin", moved, sizeof moved))
        return;
    uint32_t *piece = malloc(0x10000);
    if (!piece)
        return;

    const unsigned pieces = 128;    /* 8 MB */
    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (f) {
        const uint64_t start = armGetSystemTick();
        ok = ftruncate(fileno(f), (off_t)pieces * 0x10000) == 0;
        for (unsigned i = 0; ok && i < pieces; i++) {
            for (unsigned w = 0; w < 0x4000; w++)
                piece[w] = i * 0x4000 + w;
            ok = fwrite(piece, 1, 0x10000, f) == 0x10000;
        }
        ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
        fclose(f);
        out->write_mbps = rate((uint64_t)pieces * 0x10000, us_since(start));
    }
    if (ok && (f = fopen(path, "rb")) != NULL) {
        struct stat st;
        ok = fstat(fileno(f), &st) == 0 && st.st_size == (off_t)pieces * 0x10000;
        for (unsigned i = 0; ok && i < pieces; i++)
            ok = fread(piece, 1, 0x10000, f) == 0x10000 && piece[0] == i * 0x4000 && piece[0x3FFF] == i * 0x4000 + 0x3FFF;
        fclose(f);
    }
    ok = ok && rename(path, moved) == 0 && remove(moved) == 0;
    remove(path);
    out->write_ok = ok;
    free(piece);
    nx_log("files: the cache's way of writing (size set first, 8 MB in 64 KB pieces, read back, renamed, removed) %s, %u MB/s\n",
           ok ? "works" : "FAILED", out->write_mbps);
}

/* ---- what is decrypted on the PC ------------------------------------------------------- */

/* the folders the 26 encrypted script files are in */
static const char *const script_folders[] = {"chr", "chr/c0000", "script", "script/talk", "menu"};

static void check_decrypted(NxVfsProbe *out)
{
    char path[768];
    uint8_t head[4];
    uint64_t size;

    out->eboot = peek(NX_GAME_DIR "/PS3_GAME/USRDIR/EBOOT.elf", head, &size) && size == EBOOT_ELF_SIZE &&
                 memcmp(head, "\x7f" "ELF", 4) == 0;
    /* what the lifted libsre's glue loads (PS3_LIBSRE_IMAGE), made on the PC from libsre.prx */
    out->libsre = peek(NX_FIRMWARE_DIR "/libsre_image.bin", head, &size) && size > 0;

    /* each .sdat's header gives its plain size; the copy beside it must have that size */
    for (unsigned i = 0; i < sizeof script_folders / sizeof script_folders[0]; i++) {
        char folder[512];
        snprintf(folder, sizeof folder, NX_GAME_DIR "/PS3_GAME/USRDIR/%s", script_folders[i]);
        DIR *dir = opendir(folder);
        if (!dir)
            continue;
        for (struct dirent *e; (e = readdir(dir)) != NULL;) {
            size_t n = strlen(e->d_name);
            if (n < 5 || strcmp(e->d_name + n - 5, ".sdat") != 0)
                continue;
            out->scripts_expected++;
            uint8_t npd[0x90];
            snprintf(path, sizeof path, "%s/%s", folder, e->d_name);
            FILE *f = fopen(path, "rb");
            bool header = f && fread(npd, 1, sizeof npd, f) == sizeof npd && memcmp(npd, "NPD\0", 4) == 0;
            if (f)
                fclose(f);
            struct stat st;
            snprintf(path, sizeof path, "%s/%s.unedat", folder, e->d_name);
            if (header && stat(path, &st) == 0 && (uint64_t)st.st_size == be64(npd + 0x88))
                out->scripts++;
        }
        closedir(dir);
    }
    nx_log("files: decrypted on the PC: EBOOT.elf %s, %u of %u script files, firmware/libsre_image.bin %s\n",
           out->eboot ? "found" : "not yet", out->scripts, out->scripts_expected, out->libsre ? "found" : "not yet");
}

void nx_vfs_probe(NxVfsProbe *out)
{
    memset(out, 0, sizeof *out);
    nx_log("files: /dev_bdvd and /app_home are " NX_GAME_DIR ", /dev_hdd0 " NX_HDD0_DIR ", /dev_hdd1 " NX_HDD1_DIR "\n");

    struct statvfs fs;
    if (statvfs("sdmc:/", &fs) == 0)
        out->free_mb = (uint64_t)fs.f_bavail * fs.f_frsize / MB;
    nx_log("files: %llu MB free on the SD card\n", (unsigned long long)out->free_mb);

    check_paths(out);
    check_inflate(out);
    check_reads(out);
    check_writes(out);
    check_decrypted(out);

    if (out->paths_ok != out->paths)
        out->failed = "opening the game's files";
    else if (!out->refused_ok)
        out->failed = "refusing paths out of the drives";
    else if (!out->inflate_ok)
        out->failed = "inflating DCX files";
    else if (!out->write_ok)
        out->failed = "writing the cache";
    nx_log("files: %s\n", out->failed ? out->failed : "everything works");
}
