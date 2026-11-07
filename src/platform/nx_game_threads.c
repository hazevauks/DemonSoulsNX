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

void nx_game_threads_stats(NxThreadStats *out)
{
    mutexLock(&s_lock);
    *out = (NxThreadStats){.detached = s_count, .freed = s_freed, .peak = s_peak, .lost = s_lost};
    mutexUnlock(&s_lock);
}
