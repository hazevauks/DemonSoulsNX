/* nx_guest.c -- the PS3's 4 GB address space, without svcMapPhysicalMemory.
 *
 * The recompiled code reaches guest address `ea` at vm_base + ea, anywhere in 4 GB, and the
 * runtime turns host pointers back into guest addresses by keeping their low 32 bits, so
 * vm_base sits on a 4 GB boundary. On Windows, Soulbrandt reserves the space and commits 64 KB
 * on the first touch of each piece. The first console test showed that a process started
 * through hbloader has no system resource, so the kernel refuses svcMapPhysicalMemory.
 *
 * What the process does have is its own handle, which hbloader hands over to load NROs. With
 * it, svcMapProcessCodeMemory shows heap memory at an address of our choosing in the process's
 * code region, and svcSetProcessMemoryPermission makes it read/write. That is enough: a free
 * 4 GB window on a 4 GB boundary, kept away from libnx's own mappings, heap memory mapped where
 * the game's memory is, and a fault handler that maps a 64 KB piece wherever else it touches.
 *
 * Stage 1 only tests this, and gives everything back before it exits: the NRO returns to
 * hbloader, which keeps running in this process and reuses the heap.
 */

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "nx_guest.h"
#include "nx_log.h"

#define GUEST_SIZE   0x100010000ull     /* 4 GB and a 64 KB guard for reads at the top, as Soulbrandt */
#define GUEST_ALIGN  0x100000000ull
#define PIECE        0x10000ull         /* what a fault maps, as Soulbrandt's handler commits */

/* what the probe maps up front: main memory (above the null page) and the RSX's memory */
#define MAIN_BASE    0x00010000ull
#define MAIN_SIZE    0x0FFF0000ull
#define RSX_BASE     0xC0000000ull
#define RSX_SIZE     0x10000000ull

/* then 32 MB in 64 KB pieces, one after the other, as a run of faults maps them */
#define RUN_BASE     0x20000000ull
#define RUN_PIECES   512u

#define MAX_MAPS     1024u
#define POOL_SIZE    (MAIN_SIZE + RSX_SIZE + (RUN_PIECES + 64u) * PIECE)

/* guest addresses nothing has mapped: the null page, the gaps, the stacks, the top edge */
static const uint64_t untouched[] = {
    0x00000000, 0x10000000, 0x30000000, 0x40000000, 0x7FFF0000,
    0xD0000000, 0xE0000000, 0xFFFFFFFC, 0x100000000,
};
#define UNTOUCHED (sizeof untouched / sizeof untouched[0])

typedef struct {
    uint64_t guest, src, size;
} Mapping;

static struct {
    Handle process;
    uint8_t *window;            /* set while the fault handler may map into it */
    uint8_t *pool;              /* heap memory handed out in order */
    uint64_t pool_used;
    Mapping maps[MAX_MAPS];
    unsigned map_count;
    unsigned faults;
    Result fault_rc;
} g;

static uint64_t info(InfoType type)
{
    uint64_t value = 0;
    if (R_FAILED(svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0)))
        return 0;
    return value;
}

static uint64_t ns_since(uint64_t tick)
{
    return armTicksToNs(armGetSystemTick() - tick);
}

static bool overlaps(uint64_t at, uint64_t size, uint64_t base, uint64_t length)
{
    return at < base + length && base < at + size;
}

/* A 4 GB window on a 4 GB boundary, all unmapped, in the code region (where the kernel lets
 * svcMapProcessCodeMemory map) and outside the heap, alias and stack regions. */
static uint64_t find_window(void)
{
    const uint64_t code = info(InfoType_AslrRegionAddress), code_end = code + info(InfoType_AslrRegionSize);

    for (uint64_t at = (code + GUEST_ALIGN - 1) & ~(GUEST_ALIGN - 1); at + GUEST_SIZE <= code_end; at += GUEST_ALIGN) {
        if (overlaps(at, GUEST_SIZE, info(InfoType_HeapRegionAddress), info(InfoType_HeapRegionSize)) ||
            overlaps(at, GUEST_SIZE, info(InfoType_AliasRegionAddress), info(InfoType_AliasRegionSize)) ||
            overlaps(at, GUEST_SIZE, info(InfoType_StackRegionAddress), info(InfoType_StackRegionSize)))
            continue;
        MemoryInfo mi;
        u32 page;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &page, at)) && mi.type == MemType_Unmapped &&
            mi.addr + mi.size >= at + GUEST_SIZE)
            return at;
    }
    return 0;
}

/* Heap memory from the pool, zeroed and shown read/write at guest address `guest`. The pool
 * hands memory out in order, so pieces mapped one after the other can stay one block for the
 * kernel on both sides. */
static Result map_range(uint64_t guest, uint64_t size)
{
    if (g.map_count == MAX_MAPS || g.pool_used + size > POOL_SIZE)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);

    const uint64_t src = (uint64_t)g.pool + g.pool_used;
    const uint64_t dst = (uint64_t)g.window + guest;
    memset((void *)src, 0, size);
    Result rc = svcMapProcessCodeMemory(g.process, dst, src, size);
    if (R_FAILED(rc))
        return rc;
    rc = svcSetProcessMemoryPermission(g.process, dst, size, Perm_Rw);
    if (R_FAILED(rc)) {
        svcUnmapProcessCodeMemory(g.process, dst, src, size);
        return rc;
    }
    g.pool_used += size;
    g.maps[g.map_count++] = (Mapping){guest, src, size};
    return 0;
}

/* Unmaps everything, newest first, and returns the pool to the heap. */
static Result give_back(void)
{
    uint8_t *window = g.window;
    g.window = NULL;    /* faults are no longer ours */

    Result first = 0;
    while (g.map_count) {
        const Mapping *m = &g.maps[--g.map_count];
        Result rc = svcUnmapProcessCodeMemory(g.process, (uint64_t)window + m->guest, m->src, m->size);
        if (R_FAILED(rc) && R_SUCCEEDED(first))
            first = rc;
    }
    /* heap memory still mapped somewhere must not go back to malloc */
    if (R_SUCCEEDED(first))
        free(g.pool);
    g.pool = NULL;
    g.pool_used = 0;
    return first;
}

/* Called by nx_guest_fault.s on the faulting thread, with the kernel's exception info:
 * x0..x8, lr, sp, pc, pstate, afsr0, afsr1, esr, far. The kernel lets one thread in at a time.
 * Takes no locks and does not log: the faulting thread may hold any lock, the log's too. */
int nx_guest_fault(uint32_t type, const uint8_t *exception)
{
    (void)type;
    uint32_t esr;
    uint64_t far;
    memcpy(&esr, exception + 0x6C, sizeof esr);
    memcpy(&far, exception + 0x70, sizeof far);

    /* a data abort from the program (EC 0x24) on an unmapped page (a translation fault),
     * with a valid fault address (FnV clear), inside the window */
    if (!g.window || (esr >> 26) != 0x24 || (esr & (1u << 10)) || (esr & 0x3C) != 0x04)
        return -1;
    if (far < (uint64_t)g.window || far - (uint64_t)g.window >= GUEST_SIZE)
        return -1;

    const uint64_t guest = (far - (uint64_t)g.window) & ~(PIECE - 1);
    Result rc = map_range(guest, PIECE);
    if (R_FAILED(rc)) {
        /* another thread faulted on the same piece and mapped it first */
        MemoryInfo mi;
        u32 page;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &page, (uint64_t)g.window + guest)) && mi.type != MemType_Unmapped)
            return 0;
        g.fault_rc = rc;
        return -1;
    }
    g.faults++;
    return 0;
}

static volatile uint32_t *guest_word(uint64_t guest)
{
    return (volatile uint32_t *)(g.window + guest);
}

/* Freshly mapped memory reads as zero and keeps what is written. */
static bool check_word(uint64_t guest)
{
    volatile uint32_t *p = guest_word(guest);
    const uint32_t value = 0x50533300u ^ (uint32_t)(guest >> 4);
    if (*p != 0)
        return false;
    *p = value;
    return *p == value;
}

/* The three ways the runtime will map memory. Returns the step that failed, or NULL. */
static const char *run(NxGuestProbe *out, Result *rc)
{
    uint64_t start = armGetSystemTick();
    if (R_FAILED(*rc = map_range(MAIN_BASE, MAIN_SIZE)) || R_FAILED(*rc = map_range(RSX_BASE, RSX_SIZE)))
        return "mapping main and RSX memory";
    out->map_ms = (uint32_t)(ns_since(start) / 1000000);
    if (!check_word(MAIN_BASE) || !check_word(MAIN_BASE + MAIN_SIZE - 4) ||
        !check_word(RSX_BASE) || !check_word(RSX_BASE + RSX_SIZE - 4))
        return "reading back main and RSX memory";
    nx_log("guest probe: main and RSX memory, 512 MB, mapped and zeroed in %u ms\n", out->map_ms);

    start = armGetSystemTick();
    for (unsigned i = 0; i < RUN_PIECES; i++)
        if (R_FAILED(*rc = map_range(RUN_BASE + i * PIECE, PIECE)))
            return "mapping 64 KB pieces";
    out->piece_us = (uint32_t)(ns_since(start) / 1000 / RUN_PIECES);
    if (!check_word(RUN_BASE) || !check_word(RUN_BASE + RUN_PIECES * PIECE - 4))
        return "reading back the 64 KB pieces";
    MemoryInfo mi;
    u32 page;
    const uint64_t run_at = (uint64_t)g.window + RUN_BASE;
    out->pieces_merge = R_SUCCEEDED(svcQueryMemory(&mi, &page, run_at)) && mi.addr <= run_at &&
                        mi.addr + mi.size >= run_at + RUN_PIECES * PIECE;
    nx_log("guest probe: %u pieces of 64 KB at %u us each, %s\n", RUN_PIECES, out->piece_us,
           out->pieces_merge ? "one block for the kernel" : "kept apart by the kernel");

    nx_log("guest probe: touching unmapped guest memory; if the log ends here, faults do not reach the NRO\n");
    start = armGetSystemTick();
    for (unsigned i = 0; i < UNTOUCHED; i++)
        if (!check_word(untouched[i]))
            return "reading memory mapped on a fault";
    __asm__ volatile("" ::: "memory");      /* g.faults changed under the loads above */
    const uint64_t ns = ns_since(start);
    out->faults = g.faults;
    if (g.faults != UNTOUCHED) {
        *rc = g.fault_rc;
        return "answering faults";
    }
    out->fault_us = (uint32_t)(ns / 1000 / UNTOUCHED);
    nx_log("guest probe: %u faults answered at %u us each\n", out->faults, out->fault_us);
    return NULL;
}

void nx_guest_probe(NxGuestProbe *out)
{
    memset(out, 0, sizeof *out);
    const uint64_t code = info(InfoType_AslrRegionAddress), code_end = code + info(InfoType_AslrRegionSize);
    out->address_bits = code_end <= (1ull << 32) ? 32 : code_end <= (1ull << 36) ? 36 : 39;

    g.process = envGetOwnProcessHandle();
    const bool map_code = envIsSyscallHinted(0x77) && envIsSyscallHinted(0x78);
    const bool set_perm = envIsSyscallHinted(0x73);
    out->can_map = g.process != INVALID_HANDLE && map_code && set_perm;
    nx_log("guest probe: %d-bit address space; code region %#llx-%#llx, heap region %#llx, alias region %#llx\n",
           out->address_bits, (unsigned long long)code, (unsigned long long)code_end,
           (unsigned long long)info(InfoType_HeapRegionAddress), (unsigned long long)info(InfoType_AliasRegionAddress));
    nx_log("guest probe: process handle %s, svcMapProcessCodeMemory %s, svcSetProcessMemoryPermission %s\n",
           g.process != INVALID_HANDLE ? "given" : "missing", map_code ? "allowed" : "not allowed",
           set_perm ? "allowed" : "not allowed");

    Result rc = 0;
    if (!out->can_map) {
        out->failed = "getting the process handle and syscalls";
    } else {
        virtmemLock();
        out->window = find_window();
        VirtmemReservation *reservation = out->window ? virtmemAddReservation((void *)out->window, GUEST_SIZE) : NULL;
        virtmemUnlock();

        if (!reservation) {
            out->window = 0;
            out->failed = "finding a free 4 GB window";
        } else {
            nx_log("guest probe: window at %#llx\n", (unsigned long long)out->window);
            g.pool = memalign(PIECE, POOL_SIZE);
            if (!g.pool) {
                out->failed = "taking 548 MB from the heap";
            } else {
                g.window = (uint8_t *)out->window;
                out->failed = run(out, &rc);
                Result back = give_back();
                if (R_FAILED(back) && !out->failed) {
                    rc = back;
                    out->failed = "giving the memory back (restart the game before the next NRO)";
                }
            }
            virtmemLock();
            virtmemRemoveReservation(reservation);
            virtmemUnlock();
        }
    }

    out->rc = rc;
    if (out->failed)
        nx_log("guest probe: failed at %s, result %#x\n", out->failed, rc);
    else
        nx_log("guest probe: works without svcMapPhysicalMemory; all of it given back\n");
}
