/* nx_data.c -- checks the game's folder on the SD card. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "nx_data.h"
#include "nx_log.h"
#include "nx_paths.h"

static const char *const usrdir_folders[] = {
    "chr", "facegen", "font", "map", "menu", "movie", "msg", "mtd", "obj", "other", "param",
    "paramdef", "parts", "remo", "script", "sfx", "shader", "sound",
};

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

/* Copies a text value of a PARAM.SFO into value. */
static bool sfo_text(const uint8_t *sfo, size_t size, const char *key, char *value, size_t capacity)
{
    if (size < 20 || memcmp(sfo, "\0PSF", 4) != 0)
        return false;
    uint32_t keys = le32(sfo + 8), data = le32(sfo + 12), count = le32(sfo + 16);
    for (uint32_t i = 0; i < count && 20 + (i + 1) * 16 <= size; ++i) {
        const uint8_t *entry = sfo + 20 + i * 16;
        uint32_t name = keys + le16(entry), length = le32(entry + 4), offset = data + le32(entry + 12);
        if (name >= size || offset + length > size || strncmp((const char *)sfo + name, key, size - name) != 0)
            continue;
        size_t n = length < capacity - 1 ? length : capacity - 1;
        memcpy(value, sfo + offset, n);
        value[n] = '\0';
        return true;
    }
    return false;
}

void nx_data_check(NxData *out)
{
    memset(out, 0, sizeof(*out));
    out->folders_expected = sizeof(usrdir_folders) / sizeof(usrdir_folders[0]);

    uint8_t sfo[2048];
    FILE *file = fopen(NX_GAME_DIR "/PS3_GAME/PARAM.SFO", "rb");
    if (file) {
        size_t size = fread(sfo, 1, sizeof(sfo), file);
        fclose(file);
        out->found = sfo_text(sfo, size, "TITLE_ID", out->title_id, sizeof(out->title_id));
        sfo_text(sfo, size, "APP_VER", out->version, sizeof(out->version));
        out->right_version = strcmp(out->title_id, "BLUS30443") == 0 && strcmp(out->version, "01.00") == 0;
    }

    for (int i = 0; i < out->folders_expected; ++i) {
        char path[256];
        struct stat st;
        snprintf(path, sizeof(path), NX_GAME_DIR "/PS3_GAME/USRDIR/%s", usrdir_folders[i]);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            out->folders_found++;
        else if (!out->first_missing)
            out->first_missing = usrdir_folders[i];
    }
}

void nx_data_log(const NxData *d)
{
    if (!d->found) {
        nx_log("game: no " NX_GAME_DIR "/PS3_GAME/PARAM.SFO\n");
        return;
    }
    nx_log("game: %s version %s (%s), %d of %d folders in USRDIR%s%s\n", d->title_id, d->version,
           d->right_version ? "supported" : "not supported: the port needs BLUS30443 01.00",
           d->folders_found, d->folders_expected,
           d->first_missing ? ", missing: " : "", d->first_missing ? d->first_missing : "");
}
