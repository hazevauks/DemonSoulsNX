/* nx_game_io.c -- what the game's file reads and writes cost, by where they go.
 *
 * Linked with -Wl,--wrap for newlib's _open_r, _read_r, _write_r and _close_r (CMakeLists.txt):
 * every open(), read(), write(), fopen() and fread() of the runtime comes through them. A
 * descriptor is sorted by its path as it opens -- the disc's folder, the game's cache
 * (/dev_hdd1), its saves (/dev_hdd0) -- and the bytes and the time each read and write takes are
 * added up there. The log gives them every 10 seconds (nx_game_main.c).
 *
 * The game copies what it loads into its cache, which the runtime empties at every start, as the
 * PS3 does: on the PS3 that spared the disc drive its seeks, while here both are the same SD card.
 * These figures say what the copies cost a load (stage 11).
 */

#include <reent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include <switch.h>

#include "nx_game_io.h"
#include "nx_log.h"
#include "nx_paths.h"

#define FD_MAX 1024

int __real__open_r(struct _reent *r, const char *path, int flags, int mode);
_ssize_t __real__read_r(struct _reent *r, int fd, void *buf, size_t n);
_ssize_t __real__write_r(struct _reent *r, int fd, const void *buf, size_t n);
int __real__close_r(struct _reent *r, int fd);

typedef struct {
    uint64_t opens, read_bytes, read_ns, reads, write_bytes, write_ns, writes;
} Totals;

static Totals s_totals[NX_IO_KINDS];
static uint8_t s_kind[FD_MAX];  /* 0: not counted, else 1 + NxIoKind */

static int kind_of(const char *path)
{
    if (!path)
        return -1;
    if (strstr(path, NX_GAME_DIR "/") == path)
        return NX_IO_DISC;
    if (strstr(path, NX_HDD1_DIR "/") == path)
        return NX_IO_CACHE;
    if (strstr(path, NX_HDD0_DIR "/") == path)
        return NX_IO_SAVES;
    return -1;
}

static void add(uint64_t *total, uint64_t n)
{
    __atomic_fetch_add(total, n, __ATOMIC_RELAXED);
}

int __wrap__open_r(struct _reent *r, const char *path, int flags, int mode)
{
    const int fd = __real__open_r(r, path, flags, mode);
    const int kind = kind_of(path);
    if (fd >= 0 && fd < FD_MAX) {
        __atomic_store_n(&s_kind[fd], (uint8_t)(kind + 1), __ATOMIC_RELAXED);
        if (kind >= 0)
            add(&s_totals[kind].opens, 1);
    }
    return fd;
}

static int counted(int fd)
{
    return fd >= 0 && fd < FD_MAX ? (int)__atomic_load_n(&s_kind[fd], __ATOMIC_RELAXED) - 1 : -1;
}

_ssize_t __wrap__read_r(struct _reent *r, int fd, void *buf, size_t n)
{
    const int kind = counted(fd);
    const uint64_t start = kind >= 0 ? armGetSystemTick() : 0;
    const _ssize_t got = __real__read_r(r, fd, buf, n);
    if (kind >= 0 && got > 0) {
        add(&s_totals[kind].read_ns, armTicksToNs(armGetSystemTick() - start));
        add(&s_totals[kind].read_bytes, (uint64_t)got);
        add(&s_totals[kind].reads, 1);
    }
    return got;
}

_ssize_t __wrap__write_r(struct _reent *r, int fd, const void *buf, size_t n)
{
    const int kind = counted(fd);
    const uint64_t start = kind >= 0 ? armGetSystemTick() : 0;
    const _ssize_t put = __real__write_r(r, fd, buf, n);
    if (kind >= 0 && put > 0) {
        add(&s_totals[kind].write_ns, armTicksToNs(armGetSystemTick() - start));
        add(&s_totals[kind].write_bytes, (uint64_t)put);
        add(&s_totals[kind].writes, 1);
    }
    return put;
}

int __wrap__close_r(struct _reent *r, int fd)
{
    if (fd >= 0 && fd < FD_MAX)
        __atomic_store_n(&s_kind[fd], 0, __ATOMIC_RELAXED);
    return __real__close_r(r, fd);
}

static unsigned mbps(uint64_t bytes, uint64_t ns)
{
    return ns ? (unsigned)(bytes * 1000 / ns) : 0;   /* bytes per ns * 1000 = MB/s */
}

void nx_game_io_report(void)
{
    static const char *const names[NX_IO_KINDS] = {"the disc", "the cache", "the saves"};
    static Totals last[NX_IO_KINDS];
    char line[512];
    size_t used = 0;
    bool changed = false;
    for (int k = 0; k < NX_IO_KINDS; k++) {
        Totals now;
        now.opens = __atomic_load_n(&s_totals[k].opens, __ATOMIC_RELAXED);
        now.read_bytes = __atomic_load_n(&s_totals[k].read_bytes, __ATOMIC_RELAXED);
        now.read_ns = __atomic_load_n(&s_totals[k].read_ns, __ATOMIC_RELAXED);
        now.reads = __atomic_load_n(&s_totals[k].reads, __ATOMIC_RELAXED);
        now.write_bytes = __atomic_load_n(&s_totals[k].write_bytes, __ATOMIC_RELAXED);
        now.write_ns = __atomic_load_n(&s_totals[k].write_ns, __ATOMIC_RELAXED);
        now.writes = __atomic_load_n(&s_totals[k].writes, __ATOMIC_RELAXED);
        if (memcmp(&now, &last[k], sizeof now) != 0)
            changed = true;
        last[k] = now;
        if (!now.opens && !now.read_bytes && !now.write_bytes)
            continue;
        int n = snprintf(line + used, sizeof line - used, "%s%s %llu opened, read %llu MB in %llu ms (%u MB/s, %llu reads)",
                         used ? "; " : "", names[k], (unsigned long long)now.opens,
                         (unsigned long long)(now.read_bytes >> 20), (unsigned long long)(now.read_ns / 1000000),
                         mbps(now.read_bytes, now.read_ns), (unsigned long long)now.reads);
        if (n > 0 && (used += (size_t)n) < sizeof line && now.write_bytes)
            n = snprintf(line + used, sizeof line - used, ", wrote %llu MB in %llu ms (%u MB/s, %llu writes)",
                         (unsigned long long)(now.write_bytes >> 20), (unsigned long long)(now.write_ns / 1000000),
                         mbps(now.write_bytes, now.write_ns), (unsigned long long)now.writes);
        if (n > 0 && used < sizeof line && now.write_bytes)
            used += (size_t)n;
        if (used >= sizeof line)
            used = sizeof line - 1;
    }
    if (changed && used)
        nx_log("files so far: %s\n", line);
}
