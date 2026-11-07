/* nx_game_threads.c -- detached threads, which libnx does not have.
 *
 * newlib's pthread_create (libsysbase) passes on only the stack's address and size, so every
 * thread is joinable, and its pthread_detach calls a __syscall_thread_detach that libnx leaves
 * out, so it fails with ENOSYS. A thread is given back -- its kernel handle, its stack from the
 * heap, its control block -- only when another thread joins it. The runtime's CreateThread
 * makes every thread detached and never joins one: SPURS starts a thread for each walk of a job
 * chain, and Demon's Souls walks its texture chain again and again. About a hundred threads into
 * its boot (some 55 of them that chain's), a thread could not be created any more, a walk never
 * started, and the game waited for it forever, ten seconds in.
 *
 * So a thread created detached, or detached later, goes on a list, and a reaper joins each one
 * once it has ended (-Wl,--wrap=pthread_create, CMakeLists.txt).
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <switch.h>

#include "nx_game_threads.h"
#include "nx_log.h"

#define REAP_MAX        1024
#define REAP_EVERY_NS   (100ull * 1000 * 1000)

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg);

/* libnx's own pthread: its Thread first, whose first field is the kernel handle (nx/source/runtime/newlib.c) */
struct __pthread_t {
    Thread thr;
    void *rc;
};

static Mutex s_lock;
static pthread_t s_detached[REAP_MAX];
static unsigned s_count, s_freed, s_lost, s_peak;
static bool s_reaper;

/* Every thread made here, for DESNX_CPU_REPORT: its start routine and its ticks at the last report */
#define CPU_MAX 512
typedef struct CpuEntry {
    pthread_t thread;
    void *(*start)(void *);
    u64 ticks;
    bool primed;
} CpuEntry;
static CpuEntry s_cpu[CPU_MAX];
static unsigned s_ncpu;

static void cpu_forget(pthread_t thread)   /* (under s_lock) */
{
    for (unsigned i = 0; i < s_ncpu; i++)
        if (s_cpu[i].thread == thread) {
            s_cpu[i] = s_cpu[--s_ncpu];
            return;
        }
}

static void *reaper(void *arg);

static void add(pthread_t thread)
{
    bool start = false;
    mutexLock(&s_lock);
    if (s_count < REAP_MAX)
        s_detached[s_count++] = thread;
    else
        s_lost++;
    if (s_count > s_peak)
        s_peak = s_count;
    if (!s_reaper) {
        s_reaper = true;
        start = true;
    }
    mutexUnlock(&s_lock);
    if (start) {
        pthread_t t;
        if (__real_pthread_create(&t, NULL, reaper, NULL) != 0)
            nx_log("threads: the reaper of detached threads did not start; their memory stays taken\n");
    }
}

/* newlib's pthread_detach comes here (libsysbase calls it when it is defined) */
int __syscall_thread_detach(struct __pthread_t *thread)
{
    if (!thread)
        return EINVAL;
    add(thread);
    return 0;
}

/* A thread that asks for no stack size gets at least this: the renderer's core starts its workers
 * that way (CreateThread with 0), and they may compile shaders, which glslang and SPIRV-Tools do
 * with deep recursion (stage 12c). libnx's default is far less. */
#define MIN_STACK (1u << 20)

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    size_t size = 0;
    pthread_attr_t bigger;
    if (!attr || (pthread_attr_getstacksize(attr, &size) == 0 && size < MIN_STACK)) {
        if (attr)
            bigger = *attr;
        else
            pthread_attr_init(&bigger);
        pthread_attr_setstacksize(&bigger, MIN_STACK);
        attr = &bigger;
    }
    const int rc = __real_pthread_create(thread, attr, start, arg);
    if (rc == 0) {
        mutexLock(&s_lock);
        if (s_ncpu < CPU_MAX)
            s_cpu[s_ncpu++] = (CpuEntry){.thread = *thread, .start = start};
        mutexUnlock(&s_lock);
    }
    int state = PTHREAD_CREATE_JOINABLE;
    if (rc == 0 && attr && pthread_attr_getdetachstate(attr, &state) == 0 && state == PTHREAD_CREATE_DETACHED)
        add(*thread);
    return rc;
}

static bool ended(pthread_t thread)
{
    s32 index;
    const Handle handle = thread->thr.handle;
    return R_SUCCEEDED(svcWaitSynchronization(&index, &handle, 1, 0));
}

static void *reaper(void *arg)
{
    (void)arg;
    for (;;) {
        svcSleepThread(REAP_EVERY_NS);
        for (;;) {
            pthread_t done = NULL;
            mutexLock(&s_lock);
            for (unsigned i = 0; i < s_count; i++)
                if (ended(s_detached[i])) {
                    done = s_detached[i];
                    s_detached[i] = s_detached[--s_count];
                    cpu_forget(done);
                    break;
                }
            mutexUnlock(&s_lock);
            if (!done)
                break;
            pthread_join(done, NULL);   /* it has ended: this returns at once, and frees it */
            mutexLock(&s_lock);
            s_freed++;
            mutexUnlock(&s_lock);
        }
    }
    return NULL;
}

/* DESNX_CPU_REPORT=1 (settings.txt): the busiest threads of the last window_ns, from the kernel's
 * count of each thread's ticks, as a share of one core (the game has three). A thread is named by
 * its id as the runtime's log prints it ("[THREAD n] host thread started ... hosttid=") and by
 * its start routine (nro+ offset: tools/crash_symbolize.py names it). */
void nx_game_threads_cpu_report(u64 window_ns)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DESNX_CPU_REPORT");
        on = e && *e && *e != '0';
    }
    if (!on || !window_ns)
        return;
    extern void _start(void);
    typedef struct { unsigned i; u64 ns; } Use;
    static Use use[CPU_MAX];
    unsigned n = 0;
    u64 all = 0;
    mutexLock(&s_lock);
    for (unsigned i = 0; i < s_ncpu;) {
        u64 ticks = 0;
        const Handle h = s_cpu[i].thread->thr.handle;
        Result rc = svcGetInfo(&ticks, InfoType_ThreadTickCount, h, UINT64_MAX);
        if (R_FAILED(rc))
            rc = svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, h, UINT64_MAX);
        if (R_FAILED(rc)) {   /* ended and joined elsewhere: its handle is gone */
            s_cpu[i] = s_cpu[--s_ncpu];
            continue;
        }
        const u64 ns = s_cpu[i].primed ? armTicksToNs(ticks - s_cpu[i].ticks) : 0;
        s_cpu[i].ticks = ticks;
        s_cpu[i].primed = true;
        all += ns;
        if (ns)
            use[n++] = (Use){i, ns};
        i++;
    }
    for (unsigned a = 0; a < n && a < 10; a++)   /* the ten busiest first */
        for (unsigned b = a + 1; b < n; b++)
            if (use[b].ns > use[a].ns) {
                const Use t = use[a];
                use[a] = use[b];
                use[b] = t;
            }
    char line[1024];
    int len = snprintf(line, sizeof line, "cpu: %u threads, %.0f%% of a core in all (the game has three);",
                       s_ncpu, 100.0 * (double)all / (double)window_ns);
    for (unsigned a = 0; a < n && a < 10 && len < (int)sizeof line; a++) {
        const CpuEntry *e = &s_cpu[use[a].i];
        len += snprintf(line + len, sizeof line - (size_t)len, " %.0f%% %lu (nro+0x%lx)",
                        100.0 * (double)use[a].ns / (double)window_ns,
                        (unsigned long)(uint32_t)(uintptr_t)e->thread,
                        (unsigned long)((uintptr_t)e->start - (uintptr_t)&_start));
    }
    mutexUnlock(&s_lock);
    nx_log("%s\n", line);
}

void nx_game_threads_stats(NxThreadStats *out)
{
    mutexLock(&s_lock);
    *out = (NxThreadStats){.detached = s_count, .freed = s_freed, .peak = s_peak, .lost = s_lost};
    mutexUnlock(&s_lock);
}
