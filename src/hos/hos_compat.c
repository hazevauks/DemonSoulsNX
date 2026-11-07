/* hos_compat.c -- the POSIX pieces ps3recomp's runtime needs and Horizon does not have.
 *
 * Built into the runtime library (CMakeLists.txt). Each part says what it does and what it
 * cannot do: a call that cannot work fails with an error the runtime already reports.
 */

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <malloc.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>

#include <dlfcn.h>
#include <ifaddrs.h>
#include <sys/mman.h>
#include <ucontext.h>

#include "mbedtls/platform_time.h"

#include "hos_guest.h"

#define PAGE            0x1000
#define MAX_MAPPINGS    512
#define GUEST_SPACE     0x100000000ull

/* ---- mmap: heap memory and copies of files ---------------------------------------------- */

static struct {
    void *address;
    size_t length;
} mappings[MAX_MAPPINGS];
static Mutex mappings_lock;

/* the slot of the mapping that holds [address, address + length), or -1; with the lock held */
static int find(const void *address, size_t length)
{
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        const uint8_t *start = mappings[i].address;
        if (start && (const uint8_t *)address >= start &&
            (const uint8_t *)address + length <= start + mappings[i].length)
            return i;
    }
    return -1;
}

static bool track(void *address, size_t length)
{
    mutexLock(&mappings_lock);
    int slot = -1;
    for (int i = 0; slot < 0 && i < MAX_MAPPINGS; i++)
        if (!mappings[i].address)
            slot = i;
    if (slot >= 0) {
        mappings[slot].address = address;
        mappings[slot].length = length;
    }
    mutexUnlock(&mappings_lock);
    return slot >= 0;
}

/* A file's bytes from `offset`, without moving the descriptor's own position. */
static bool read_at(int fd, void *buffer, size_t length, off_t offset)
{
    const off_t was = lseek(fd, 0, SEEK_CUR);
    if (was < 0 || lseek(fd, offset, SEEK_SET) < 0)
        return false;
    size_t done = 0;
    bool ok = true;
    while (done < length) {
        const ssize_t got = read(fd, (uint8_t *)buffer + done, length - done);
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0)
            break;      /* past the end of the file: the rest stays zero, as a mapping reads */
        done += (size_t)got;
    }
    lseek(fd, was, SEEK_SET);
    return ok;
}

void *mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset)
{
    (void)address;      /* a hint only, without MAP_FIXED */
    if (length == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if (flags & MAP_FIXED) {        /* no placing memory at a given address */
        errno = ENOSYS;
        return MAP_FAILED;
    }
    /* a reservation of address space, or one as large as the guest's 4 GB: the boot's own
     * (boot_main.cpp), which finds vm_base already set to nx_guest's window and keeps it */
    if ((flags & MAP_ANONYMOUS) && (prot == PROT_NONE || length >= GUEST_SPACE)) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    if (!(flags & MAP_ANONYMOUS) && (flags & MAP_SHARED) && (prot & PROT_WRITE)) {
        errno = ENOSYS;             /* writes to the copy would never reach the file */
        return MAP_FAILED;
    }

    const size_t size = (length + PAGE - 1) & ~(size_t)(PAGE - 1);
    void *memory = memalign(PAGE, size);
    if (!memory) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    memset(memory, 0, size);
    if (!(flags & MAP_ANONYMOUS) && !read_at(fd, memory, length, offset)) {
        const int error = errno;
        free(memory);
        errno = error;
        return MAP_FAILED;
    }
    if (!track(memory, size)) {
        free(memory);
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return memory;
}

int munmap(void *address, size_t length)
{
    (void)length;
    mutexLock(&mappings_lock);
    int slot = -1;
    for (int i = 0; i < MAX_MAPPINGS && slot < 0; i++)
        if (mappings[i].address == address)
            slot = i;
    if (slot >= 0)
        mappings[slot].address = NULL;
    mutexUnlock(&mappings_lock);
    if (slot < 0) {                 /* only whole mappings go back */
        errno = EINVAL;
        return -1;
    }
    free(address);
    return 0;
}

/* ---- the guest's 4 GB: src/platform/nx_guest.c maps it ---------------------------------- */

static uintptr_t guest_window;
static uint64_t guest_size;
static uint32_t (*guest_commit)(uint64_t guest, uint64_t size);

void hos_guest_window(void *window, uint64_t size, uint32_t (*commit)(uint64_t guest, uint64_t size))
{
    guest_size = size;
    guest_commit = commit;
    __atomic_store_n(&guest_window, (uintptr_t)window, __ATOMIC_RELEASE);
}

/* Accepted on memory mmap handed out, but not enforced: the heap has no page protection here.
 * In the guest's window, making memory readable or writable is how the runtime commits it
 * (vm_commit, VirtualAlloc's MEM_COMMIT): nx_guest maps it there and then, in one piece. Taking
 * access away (a stack's guard page, a decommit) is accepted and not enforced either. */
int mprotect(void *address, size_t length, int prot)
{
    const uintptr_t window = __atomic_load_n(&guest_window, __ATOMIC_ACQUIRE);
    if (window && (uintptr_t)address >= window && (uintptr_t)address - window < guest_size) {
        if (!(prot & (PROT_READ | PROT_WRITE)) || length == 0)
            return 0;
        if (guest_commit((uintptr_t)address - window, length) != 0) {
            errno = ENOMEM;
            return -1;
        }
        return 0;
    }

    mutexLock(&mappings_lock);
    const int slot = find(address, length);
    mutexUnlock(&mappings_lock);
    if (slot < 0) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

int madvise(void *address, size_t length, int advice)
{
    (void)address;
    (void)length;
    (void)advice;
    return 0;
}

int msync(void *address, size_t length, int flags)
{
    (void)address;
    (void)length;
    (void)flags;
    return 0;
}

/* ---- dlfcn: one NRO, nothing to load ---------------------------------------------------- */

int dladdr(const void *address, Dl_info *info)
{
    (void)address;
    (void)info;
    return 0;
}

void *dlopen(const char *file, int mode)
{
    (void)file;
    (void)mode;
    return NULL;
}

void *dlsym(void *handle, const char *name)
{
    (void)handle;
    (void)name;
    return NULL;
}

int dlclose(void *handle)
{
    (void)handle;
    return 0;
}

char *dlerror(void)
{
    static char message[] = "Horizon loads no shared objects";
    return message;
}

/* ---- ucontext: not yet (cellFiber only) ------------------------------------------------- */

int getcontext(ucontext_t *context)
{
    (void)context;
    errno = ENOSYS;
    return -1;
}

int setcontext(const ucontext_t *context)
{
    (void)context;
    errno = ENOSYS;
    return -1;
}

int swapcontext(ucontext_t *from, const ucontext_t *to)
{
    (void)from;
    (void)to;
    errno = ENOSYS;
    return -1;
}

void makecontext(ucontext_t *context, void (*fn)(void), int argc, ...)
{
    (void)context;
    (void)fn;
    (void)argc;
}

/* ---- ifaddrs: no interface list --------------------------------------------------------- */

int getifaddrs(struct ifaddrs **list)
{
    *list = NULL;
    errno = ENOSYS;
    return -1;
}

void freeifaddrs(struct ifaddrs *list)
{
    (void)list;
}

/* ---- libc: declared by newlib, not implemented under libnx ----------------------------- */

/* The two the runtime asks: the page size, and the cores the process may run threads on. */
long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE:
        return PAGE;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: {
        uint64_t mask = 0;
        if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || mask == 0)
            return 1;
        return __builtin_popcountll(mask);
    }
    default:
        errno = EINVAL;
        return -1;
    }
}

/* No pipes on Horizon. The one caller, the PPU loader's check that guest memory is readable
 * before it dumps it, reads as it would without the check. */
int pipe(int fds[2])
{
    (void)fds;
    errno = ENOSYS;
    return -1;
}

/* glob() for a pattern whose wildcards are all in its last part, which is how the renderer's
 * core asks (rsxc_frame.inc: is there a shot_NNN_00_d*.bmp yet). Its flags are left out; the
 * names come back in the folder's order. */
static int glob_add(glob_t *g, const char *path)
{
    char **list = realloc(g->gl_pathv, (size_t)(g->gl_pathc + 2) * sizeof *list);
    if (!list)
        return -1;
    g->gl_pathv = list;
    if (!(list[g->gl_pathc] = strdup(path)))
        return -1;
    list[++g->gl_pathc] = NULL;
    return 0;
}

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g)
{
    (void)flags;
    (void)errfunc;
    memset(g, 0, sizeof *g);
    const char *slash = strrchr(pattern, '/');
    char dir[1024];
    const size_t n = slash ? (size_t)(slash - pattern) : 0;
    if (n >= sizeof dir)
        return GLOB_NOSPACE;
    memcpy(dir, pattern, n);
    dir[n] = '\0';
    const char *name = slash ? slash + 1 : pattern;
    DIR *d = opendir(slash ? (n ? dir : "/") : ".");
    if (!d)
        return GLOB_NOMATCH;
    char path[1280];
    for (const struct dirent *e; (e = readdir(d));) {
        if (fnmatch(name, e->d_name, 0) != 0)
            continue;
        snprintf(path, sizeof path, "%s%s%s", slash ? dir : "", slash ? "/" : "", e->d_name);
        if (glob_add(g, path) != 0) {
            closedir(d);
            return GLOB_NOSPACE;
        }
    }
    closedir(d);
    return g->gl_pathc ? 0 : GLOB_NOMATCH;
}

void globfree(glob_t *g)
{
    for (size_t i = 0; g->gl_pathv && i < (size_t)g->gl_pathc; i++)
        free(g->gl_pathv[i]);
    free(g->gl_pathv);
    g->gl_pathv = NULL;
    g->gl_pathc = 0;
}

int posix_memalign(void **out, size_t alignment, size_t size)
{
    if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
        return EINVAL;
    void *memory = memalign(alignment, size ? size : 1);
    if (!memory)
        return ENOMEM;
    *out = memory;
    return 0;
}

/* ---- time ------------------------------------------------------------------------------- */

/* A broken-down UTC time as seconds since 1970: days from the civil date by the usual era
 * arithmetic, with no tables and no time zone. */
time_t timegm(struct tm *tm)
{
    long long year = tm->tm_year + 1900LL;
    long long month = tm->tm_mon;               /* 0-11, possibly out of range */
    year += month / 12;
    month %= 12;
    if (month < 0) {
        month += 12;
        year--;
    }
    month += 1;                                 /* 1-12 */
    year -= month <= 2;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const long long year_of_era = year - era * 400;
    const long long day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + tm->tm_mday - 1;
    const long long day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    const long long days = era * 146097 + day_of_era - 719468;
    return (time_t)(((days * 24 + tm->tm_hour) * 60 + tm->tm_min) * 60 + tm->tm_sec);
}

/* pthread_mutex_timedlock: try, and sleep 100 us between tries, until the deadline (CLOCK_REALTIME). */
int hos_pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline)
{
    for (;;) {
        const int result = pthread_mutex_trylock(mutex);
        if (result != EBUSY)
            return result;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline->tv_sec || (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec))
            return ETIMEDOUT;
        svcSleepThread(100 * 1000);
    }
}

/* ---- mbedTLS's platform hooks (hos_mbedtls_config.h) ------------------------------------ */

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t length, size_t *written)
{
    (void)data;
    randomGet(output, length);      /* the kernel's random source */
    *written = length;
    return 0;
}

mbedtls_ms_time_t mbedtls_ms_time(void)
{
    return (mbedtls_ms_time_t)(armTicksToNs(armGetSystemTick()) / 1000000);
}
