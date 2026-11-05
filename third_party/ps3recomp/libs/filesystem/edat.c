/* edat.c -- NPDRM (EDAT/SDAT) files, served from the player's own decrypted copies.
 *
 * DemonSoulsNX: this copy of ps3recomp carries no keys and decrypts nothing. Upstream's
 * edat.c decrypts SDATA with the platform's published constants; here that code and its
 * constants are removed. The player decrypts the game's .sdat files once on their PC with
 * their own RPCS3 (Utilities > Decrypt PS3 Binaries), and the runtime opens those copies.
 *
 * Where a copy is looked for, for an NPD file at host_path:
 *   1. with PS3_EDAT_PLAIN=<folder>, the file at host_path's path below PS3_VFS_ROOT in that
 *      folder, named "<name>.unedat" (RPCS3's name for its output), as upstream does;
 *   2. "<name>.unedat" beside the file (how tools/make_sd.py lays out the SD card);
 *   3. "<name>.dec" beside the file (a copy upstream's runtime made earlier).
 * A copy is used only when its size is the plaintext size the NPD header gives.
 */

#include "edat.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint32_t be32(const uint8_t* p)
{ return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

int edat_selftest(void)
{
    return 0;   /* nothing to check: no cryptography in this build */
}

int edat_is_npd(const char* host_path)
{
    FILE* f = fopen(host_path, "rb");
    if (!f) return 0;
    uint8_t m[4] = {0};
    size_t n = fread(m, 1, 4, f);
    fclose(f);
    return n == 4 && m[0] == 'N' && m[1] == 'P' && m[2] == 'D' && m[3] == 0;
}

int edat_decrypt_file(const char* in_path, const char* out_path)
{
    (void)out_path;
    fprintf(stderr, "[edat] %s is still encrypted, and this build decrypts nothing: decrypt it on your PC with "
                    "RPCS3 (Utilities > Decrypt PS3 Binaries) and put the .unedat file beside it\n", in_path);
    return -1;
}

/* The plaintext's size, as the NPD file's header gives it. 0 if it cannot be read. */
static int edat_plain_size(const char* host_path, uint64_t* size)
{
    FILE* f = fopen(host_path, "rb");
    if (!f) return 0;
    uint8_t hdr[0x90];
    const size_t n = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    if (n != sizeof hdr) return 0;
    *size = be64(hdr + 0x88);
    return 1;
}

static int has_size(const char* path, uint64_t size)
{
    struct stat st;
    return stat(path, &st) == 0 && (uint64_t)st.st_size == size;
}

/* Case 1 above: the copy at host_path's path below PS3_VFS_ROOT, inside PS3_EDAT_PLAIN. */
static int edat_plain_folder(const char* host_path, uint64_t size, char* buf, size_t cap)
{
    const char* plain = getenv("PS3_EDAT_PLAIN");
    const char* root = getenv("PS3_VFS_ROOT");
    if (!plain || !plain[0] || !root || !root[0]) return 0;
    size_t n = strlen(root);
    while (n && (root[n - 1] == '/' || root[n - 1] == '\\')) --n;
    for (size_t i = 0; i < n; ++i) {                   /* (either separator; on Windows, either case) */
        int a = (unsigned char)host_path[i], b = (unsigned char)root[i];
        if (!a) return 0;
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
#ifdef _WIN32
        a = tolower(a);
        b = tolower(b);
#endif
        if (a != b) return 0;
    }
    if (host_path[n] != '/' && host_path[n] != '\\') return 0;
    const int len = snprintf(buf, cap, "%s/%s.unedat", plain, host_path + n + 1);
    return len > 0 && (size_t)len < cap && has_size(buf, size);
}

/* Cases 2 and 3 above: a copy beside the file. */
static int edat_beside(const char* host_path, const char* suffix, uint64_t size, char* buf, size_t cap)
{
    const int len = snprintf(buf, cap, "%s%s", host_path, suffix);
    return len > 0 && (size_t)len < cap && has_size(buf, size);
}

const char* edat_resolve(const char* host_path, char* buf, size_t cap)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char* e = getenv("PS3_EDAT");
        enabled = (e && e[0] == '0') ? 0 : 1;
    }
    uint64_t size;
    if (!enabled || !host_path || !edat_is_npd(host_path) || !edat_plain_size(host_path, &size)) return host_path;
    if (edat_plain_folder(host_path, size, buf, cap)) return buf;
    if (edat_beside(host_path, ".unedat", size, buf, cap)) return buf;
    if (edat_beside(host_path, ".dec", size, buf, cap)) return buf;
    edat_decrypt_file(host_path, NULL);                /* only says what is missing */
    return host_path;
}
