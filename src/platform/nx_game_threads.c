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
#include <sys/stat.h>

#include <switch.h>

#include "nx_game_threads.h"
#include "nx_log.h"
#include "nx_paths.h"

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

/* The thread pointer in TPIDR_EL0 as well (stage 15). The lifted SPU images are built with
 * -mtp=tpidr_el0 (CMakeLists.txt), so their thread-local reads -- the trampoline check after
 * every call -- are one mrs instead of a call to libnx's __aarch64_read_tp, which reads the same
 * pointer from the thread's ThreadVars (TPIDRRO_EL0 + 0x1F8). Horizon keeps TPIDR_EL0 for each
 * thread and nothing else here uses it. Every thread made through pthread_create sets it before
 * its start routine runs, and the main thread at the start (nx_game_main.c). */
void nx_thread_pointer_set(void)
{
    extern void *__aarch64_read_tp(void);
    void *const tp = __aarch64_read_tp();
    __asm__ volatile("msr tpidr_el0, %0" : : "r"(tp));
}

typedef struct { void *(*start)(void *); void *arg; } Start;

static void *started(void *p)
{
    const Start s = *(Start *)p;
    free(p);
    nx_thread_pointer_set();
    return s.start(s.arg);
}

int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start)(void *), void *arg)
{
    Start *s = malloc(sizeof *s);
    if (!s)
        return EAGAIN;
    *s = (Start){start, arg};
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
    const int rc = __real_pthread_create(thread, attr, started, s);
    if (rc != 0)
        free(s);
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

/* ---------------------------------------------------------------------------
 * The sampling profiler
 *
 * DESNX_PROFILE=1 (settings.txt): every second the six threads that used the most CPU in it are
 * picked, and every 2 ms each is paused for an instant (svcSetThreadActivity) and its pc and lr
 * read (svcGetThreadContext3). Every 15 seconds the samples go to SD_LOG/profile/<n>.bin with the
 * threads they came from; tools/profile_report.py names the functions from the game's ELF. A
 * paused thread is let go before anything else is done, so no lock it holds is ever waited on.
 * The profiler's own thread is made with __real_pthread_create, so it is not on the list.
 * -------------------------------------------------------------------------*/

#define PROF_TOP       6
#define PROF_TICK_NS   (2ull * 1000 * 1000)
#define PROF_WINDOW    7500u          /* ticks: 15 s */
#define PROF_SAMPLES   65536u
#define PROF_THREADS   64u

typedef struct { uint32_t thread, pad; uint64_t pc, lr; } ProfSample;
typedef struct { uint32_t id, pad; uint64_t start; } ProfThread;

static ProfSample s_prof[PROF_SAMPLES];
static ProfThread s_prof_threads[PROF_THREADS];

/* The busiest threads since the last call, by the kernel's ticks: their handles and slots in the
 * window's table (new ones are added to it). */
static unsigned prof_pick(Handle *h, uint32_t *slot, unsigned *nthreads)
{
    static u64 last[CPU_MAX];
    static pthread_t who[CPU_MAX];
    typedef struct { Handle h; u64 used; uint32_t id; uint64_t start; } Pick;
    Pick pick[PROF_TOP];
    unsigned npick = 0;
    mutexLock(&s_lock);
    for (unsigned i = 0; i < s_ncpu; i++) {
        u64 ticks = 0;
        const Handle th = s_cpu[i].thread->thr.handle;
        if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, th, UINT64_MAX)) &&
            R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, th, UINT64_MAX)))
            continue;
        const u64 used = who[i] == s_cpu[i].thread ? ticks - last[i] : 0;
        who[i] = s_cpu[i].thread;
        last[i] = ticks;
        const Pick p = {th, used, (uint32_t)(uintptr_t)s_cpu[i].thread,
                        (uint64_t)(uintptr_t)s_cpu[i].start};
        unsigned at = npick < PROF_TOP ? npick++ : PROF_TOP;
        if (at == PROF_TOP) {   /* full: replace the least busy if this one used more */
            unsigned least = 0;
            for (unsigned k = 1; k < PROF_TOP; k++)
                if (pick[k].used < pick[least].used) least = k;
            if (pick[least].used >= used) continue;
            at = least;
        }
        pick[at] = p;
    }
    mutexUnlock(&s_lock);
    for (unsigned k = 0; k < npick; k++) {
        unsigned s = 0;
        while (s < *nthreads && s_prof_threads[s].id != pick[k].id) s++;
        if (s == *nthreads && *nthreads < PROF_THREADS)
            s_prof_threads[(*nthreads)++] = (ProfThread){pick[k].id, 0, pick[k].start};
        h[k] = pick[k].h;
        slot[k] = s < PROF_THREADS ? s : PROF_THREADS - 1;
    }
    return npick;
}

static void prof_write(unsigned window, unsigned nthreads, unsigned nsamples, u64 ns)
{
    extern void _start(void);
    extern char __end__[];
    char path[96];
    mkdir(NX_LOG_DIR "/profile", 0777);
    snprintf(path, sizeof path, NX_LOG_DIR "/profile/%u.bin", window);
    FILE *f = fopen(path, "wb");
    if (!f) {
        nx_log("profile: %s could not be written\n", path);
        return;
    }
    const uint64_t head[3] = {(uint64_t)(uintptr_t)&_start, (uint64_t)(uintptr_t)__end__, ns};
    const uint32_t counts[4] = {window, nthreads, nsamples, 0};
    fwrite("DSNXPRF1", 1, 8, f);
    fwrite(head, sizeof head, 1, f);
    fwrite(counts, sizeof counts, 1, f);
    fwrite(s_prof_threads, sizeof s_prof_threads[0], nthreads, f);
    fwrite(s_prof, sizeof s_prof[0], nsamples, f);
    fclose(f);
    nx_log("profile: window %u, %u samples of %u threads, in %s\n", window, nsamples, nthreads, path);
}

static u64 thread_ticks(Handle h)
{
    u64 t = 0;
    if (R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCount, h, UINT64_MAX)) &&
        R_FAILED(svcGetInfo(&t, InfoType_ThreadTickCountDeprecated, h, UINT64_MAX)))
        return 0;
    return t;
}

/* A thread is paused only when it ran for at least half the time since the last sample: one
 * that is waiting in the kernel is left alone. The first profile paused waiting threads too,
 * and the game stopped for good at a loading screen (2026-11-07). */
static void *profiler(void *arg)
{
    (void)arg;
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x1C);
    Handle h[PROF_TOP];
    uint32_t slot[PROF_TOP];
    u64 seen[PROF_TOP], last_at = armGetSystemTick();
    unsigned npick = 0, nthreads = 0, nsamples = 0, window = 0, warned = 0;
    u64 t0 = armGetSystemTick();
    for (unsigned tick = 0;; tick++) {
        if (tick % 500 == 0) {   /* each second: the busiest threads of the last one */
            npick = prof_pick(h, slot, &nthreads);
            for (unsigned k = 0; k < npick; k++) seen[k] = thread_ticks(h[k]);
            last_at = armGetSystemTick();
        }
        const u64 now = armGetSystemTick(), elapsed = now - last_at;
        last_at = now;
        for (unsigned k = 0; k < npick && nsamples < PROF_SAMPLES; k++) {
            const u64 tk = thread_ticks(h[k]);
            const u64 ran = tk - seen[k];
            seen[k] = tk;
            if (!tk || ran * 2 < elapsed)   /* mostly not running: maybe waiting in the kernel */
                continue;
            ThreadContext ctx;
            if (R_FAILED(svcSetThreadActivity(h[k], ThreadActivity_Paused)))
                continue;
            const Result rc = svcGetThreadContext3(&ctx, h[k]);
            svcSetThreadActivity(h[k], ThreadActivity_Runnable);
            if (R_FAILED(rc)) {
                if (!warned++) nx_log("profile: a thread's registers could not be read (%#x)\n", rc);
                continue;
            }
            s_prof[nsamples++] = (ProfSample){slot[k], 0, ctx.pc.x, ctx.lr};
        }
        if (tick % PROF_WINDOW == PROF_WINDOW - 1) {
            const u64 t1 = armGetSystemTick();
            prof_write(++window, nthreads, nsamples, armTicksToNs(t1 - t0));
            t0 = t1;
            nthreads = nsamples = 0;
            npick = prof_pick(h, slot, &nthreads);
            for (unsigned k = 0; k < npick; k++) seen[k] = thread_ticks(h[k]);
            last_at = armGetSystemTick();
        }
        svcSleepThread(PROF_TICK_NS);
    }
    return NULL;
}

void nx_game_threads_profile_start(void)
{
    const char *e = getenv("DESNX_PROFILE");
    if (!e || !*e || *e == '0')
        return;
    pthread_t t;
    if (__real_pthread_create(&t, NULL, profiler, NULL) != 0) {
        nx_log("profile: the profiler's thread did not start\n");
        return;
    }
    nx_log("profile: sampling the six busiest threads every 2 ms, a file every 15 s (DESNX_PROFILE)\n");
}

void nx_game_threads_stats(NxThreadStats *out)
{
    mutexLock(&s_lock);
    *out = (NxThreadStats){.detached = s_count, .freed = s_freed, .peak = s_peak, .lost = s_lost};
    mutexUnlock(&s_lock);
}
