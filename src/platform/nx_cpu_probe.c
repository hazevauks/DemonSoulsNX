/* nx_cpu_probe.c -- stage 3: the cores the game's threads get, and how fast they wake each other.
 *
 * On a PC, Soulbrandt keeps about 3.8 cores busy in a level (its docs/28): two SPU executor
 * threads, HighGraphics, the vblank ticker, and threads that wait in short sleeps and condition
 * variables. The Switch gives an application three cores. What decides how those threads share
 * them is measured here:
 *   - which cores and priorities the process may use, and how fast each core runs the same work;
 *   - how long a thread takes to wake another: through address arbitration (svcWaitForAddress,
 *     the kernel's futex, which the runtime's WaitOnAddress can map onto) and through libnx's
 *     CondVar (what pthreads, and so the runtime's POSIX layer today, use);
 *   - how close a 1 ms sleep comes to 1 ms (the game's sys_timer_usleep), and what a yield costs.
 */

#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "nx_cpu.h"
#include "nx_log.h"

#define ROUND_TRIPS 2000
#define PRIORITY    0x2C            /* hbloader's main thread's */
#define STACK       0x8000

static uint64_t info(InfoType type)
{
    uint64_t value = 0;
    if (R_FAILED(svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0)))
        return 0;
    return value;
}

static uint64_t us_since(uint64_t tick)
{
    return armTicksToNs(armGetSystemTick() - tick) / 1000;
}

/* Runs fn(arg) on a thread of its own on `core`; false if the core is not allowed. */
static bool run_on(int core, ThreadFunc fn, void *arg, Thread *thread)
{
    if (R_FAILED(threadCreate(thread, fn, arg, NULL, STACK, PRIORITY, core)))
        return false;
    if (R_FAILED(threadStart(thread))) {
        threadClose(thread);
        return false;
    }
    return true;
}

static void finish(Thread *thread)
{
    threadWaitForExit(thread);
    threadClose(thread);
}

/* ---- the same work on each core ------------------------------------------------------ */

static volatile uint64_t sink;

static void work(void *arg)
{
    uint32_t *ms = arg;
    const uint64_t start = armGetSystemTick();
    uint64_t x = 88172645463325252ull;
    for (int i = 0; i < 20 * 1000 * 1000; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
    }
    sink = x;
    *ms = (uint32_t)(us_since(start) / 1000);
}

/* ---- wakes: one thread hands a turn to the other and waits for it back -------------------- */

static int32_t turn;
static Mutex lock;
static CondVar changed;

static int32_t load(void)
{
    return __atomic_load_n(&turn, __ATOMIC_ACQUIRE);
}

static void hand(int32_t value)
{
    __atomic_store_n(&turn, value, __ATOMIC_RELEASE);
    svcSignalToAddress(&turn, SignalType_Signal, 0, 1);
}

static void wait_while(int32_t value)
{
    while (load() == value)
        svcWaitForAddress(&turn, ArbitrationType_WaitIfEqual, value, -1);
}

static void ping_address(void *arg)
{
    (void)arg;
    for (int i = 0; i < ROUND_TRIPS; i++) {
        hand(1);
        wait_while(1);
    }
}

static void pong_address(void *arg)
{
    (void)arg;
    for (int i = 0; i < ROUND_TRIPS; i++) {
        wait_while(0);
        hand(0);
    }
}

static void ping_condvar(void *arg)
{
    (void)arg;
    mutexLock(&lock);
    for (int i = 0; i < ROUND_TRIPS; i++) {
        turn = 1;
        condvarWakeOne(&changed);
        while (turn == 1)
            condvarWait(&changed, &lock);
    }
    mutexUnlock(&lock);
}

static void pong_condvar(void *arg)
{
    (void)arg;
    mutexLock(&lock);
    for (int i = 0; i < ROUND_TRIPS; i++) {
        while (turn == 0)
            condvarWait(&changed, &lock);
        turn = 0;
        condvarWakeOne(&changed);
    }
    mutexUnlock(&lock);
}

/* Time per wake, in ns: each round trip is two. 0 if a thread could not be made. */
static uint32_t ping_pong(ThreadFunc ping, ThreadFunc pong, int ping_core, int pong_core)
{
    Thread a, b;
    turn = 0;
    if (R_FAILED(threadCreate(&b, pong, NULL, NULL, STACK, PRIORITY, pong_core)))
        return 0;
    if (R_FAILED(threadCreate(&a, ping, NULL, NULL, STACK, PRIORITY, ping_core))) {
        threadClose(&b);
        return 0;
    }
    const uint64_t start = armGetSystemTick();
    threadStart(&b);
    threadStart(&a);
    finish(&a);
    finish(&b);
    return (uint32_t)(armTicksToNs(armGetSystemTick() - start) / (2 * ROUND_TRIPS));
}

void nx_cpu_probe(NxCpuProbe *out)
{
    memset(out, 0, sizeof *out);
    out->core_mask = info(InfoType_CoreMask);
    out->priority_mask = info(InfoType_PriorityMask);

    char cores[96] = "";
    for (int core = 0; core < 4; core++) {
        Thread thread;
        if (!(out->core_mask & (1ull << core)) || !run_on(core, work, &out->core_ms[core], &thread)) {
            snprintf(cores + strlen(cores), sizeof cores - strlen(cores), "%score %d not allowed", core ? ", " : "", core);
            continue;
        }
        finish(&thread);
        out->cores++;
        snprintf(cores + strlen(cores), sizeof cores - strlen(cores), "%score %d %u ms", core ? ", " : "", core,
                 out->core_ms[core]);
    }
    nx_log("cpu: core mask %#llx, priority mask %#llx; the same work alone: %s\n",
           (unsigned long long)out->core_mask, (unsigned long long)out->priority_mask, cores);

    mutexInit(&lock);
    condvarInit(&changed);
    out->wake_cross_ns = ping_pong(ping_address, pong_address, 0, 1);
    out->wake_same_ns = ping_pong(ping_address, pong_address, 1, 1);
    out->condvar_cross_ns = ping_pong(ping_condvar, pong_condvar, 0, 1);
    nx_log("cpu: a wake takes %u ns across cores and %u ns on one core (address arbitration), %u ns across cores "
           "with a condition variable\n", out->wake_cross_ns, out->wake_same_ns, out->condvar_cross_ns);

    uint64_t total = 0, worst = 0;
    for (int i = 0; i < 100; i++) {
        const uint64_t start = armGetSystemTick();
        svcSleepThread(1000 * 1000);
        const uint64_t us = us_since(start);
        total += us;
        if (us > worst)
            worst = us;
    }
    out->sleep_us = (uint32_t)(total / 100);
    out->sleep_worst_us = (uint32_t)worst;

    const uint64_t start = armGetSystemTick();
    for (int i = 0; i < 1000; i++)
        svcSleepThread(0);
    out->yield_ns = (uint32_t)(armTicksToNs(armGetSystemTick() - start) / 1000);
    nx_log("cpu: a 1 ms sleep takes %u us, the longest %u us; a yield %u ns\n", out->sleep_us, out->sleep_worst_us,
           out->yield_ns);
}
