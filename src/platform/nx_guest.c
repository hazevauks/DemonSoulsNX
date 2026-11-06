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
 * the runtime commits memory (vm_commit's mprotect, src/hos/hos_compat.c), and a fault handler
 * that maps a piece wherever else the game touches.
 *
 * The kernel keeps each mapping a memory block of its own, even beside another (the second
 * console test), and it has only so many: commits map a whole range at once, and a fault maps
 * 1 MB, not Soulbrandt's 64 KB. The memory comes from the heap; nx_guest_close gives it back,
 * as hbloader goes on running in this process after the NRO exits.
 */

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "nx_guest.h"
#include "nx_log.h"

#define GUEST_ALIGN  0x100000000ull
#define PIECE        NX_GUEST_PIECE
#define PIECES       (NX_GUEST_SIZE / PIECE)
#define PAGE_64K     0x10000ull         /* the runtime's page bitmap: a bit for each 64 KB */
#define MAX_MAPS     8192u

typedef struct {
    uint64_t guest, src, size;
} Mapping;

static struct {
    Handle process;
    VirtmemReservation *reservation;
    uint8_t *volatile window;   /* set while faults in it are ours */
    uint8_t *page_bitmap;       /* the runtime's, kept in step, or NULL */
    Mutex lock;                 /* the mappings: commits and faults */
    uint8_t mapped[(PIECES + 7) / 8];
    Mapping maps[MAX_MAPS];
    unsigned map_count;
    uint64_t mapped_bytes;
    unsigned faults;
    unsigned refused;
    Result refused_rc;
    uint64_t refused_at;
    /* what nx_guest_open found, for the probe's screen */
    int address_bits;
    bool can_map;
    const char *open_failed;
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

static bool is_mapped(uint64_t piece)
{
    return g.mapped[piece / 8] & (1u << (piece % 8));
}

/* A 4 GB window on a 4 GB boundary, all unmapped, in the code region (where the kernel lets
 * svcMapProcessCodeMemory map) and outside the heap, alias and stack regions. */
static uint64_t find_window(void)
{
    const uint64_t code = info(InfoType_AslrRegionAddress), code_end = code + info(InfoType_AslrRegionSize);

    for (uint64_t at = (code + GUEST_ALIGN - 1) & ~(GUEST_ALIGN - 1); at + NX_GUEST_SIZE <= code_end; at += GUEST_ALIGN) {
        if (overlaps(at, NX_GUEST_SIZE, info(InfoType_HeapRegionAddress), info(InfoType_HeapRegionSize)) ||
            overlaps(at, NX_GUEST_SIZE, info(InfoType_AliasRegionAddress), info(InfoType_AliasRegionSize)) ||
            overlaps(at, NX_GUEST_SIZE, info(InfoType_StackRegionAddress), info(InfoType_StackRegionSize)))
            continue;
        MemoryInfo mi;
        u32 page;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &page, at)) && mi.type == MemType_Unmapped &&
            mi.addr + mi.size >= at + NX_GUEST_SIZE)
            return at;
    }
    return 0;
}

/* Heap memory, zeroed and shown read/write at pieces [first, first + count). With the lock held;
 * takes no other lock but malloc's, and touches nothing in the window, so a fault can call it. */
static Result map_run(uint64_t first, uint64_t count)
{
    if (g.map_count == MAX_MAPS)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    const uint64_t guest = first * PIECE, size = count * PIECE;
    void *src = memalign(0x1000, size);
    if (!src)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    memset(src, 0, size);

    const uint64_t dst = (uint64_t)g.window + guest;
    Result rc = svcMapProcessCodeMemory(g.process, dst, (uint64_t)src, size);
    if (R_FAILED(rc)) {
        free(src);
        return rc;
    }
    rc = svcSetProcessMemoryPermission(g.process, dst, size, Perm_Rw);
    if (R_FAILED(rc)) {
        /* heap memory still mapped somewhere must not go back to malloc */
        if (R_SUCCEEDED(svcUnmapProcessCodeMemory(g.process, dst, (uint64_t)src, size)))
            free(src);
        return rc;
    }

    g.maps[g.map_count++] = (Mapping){guest, (uint64_t)src, size};
    g.mapped_bytes += size;
    for (uint64_t p = first; p < first + count; p++)
        g.mapped[p / 8] |= (uint8_t)(1u << (p % 8));
    if (g.page_bitmap)
        for (uint64_t p = guest / PAGE_64K; p < (guest + size) / PAGE_64K && p < GUEST_ALIGN / PAGE_64K; p++)
            g.page_bitmap[p / 8] |= (uint8_t)(1u << (p % 8));
    return 0;
}

static void note_refusal(Result rc, uint64_t guest)
{
    if (!g.refused++) {
        g.refused_rc = rc;
        g.refused_at = guest;
    }
}

uint8_t *nx_guest_open(uint8_t *page_bitmap)
{
    if (g.window)
        return g.window;
    const uint64_t code = info(InfoType_AslrRegionAddress), code_end = code + info(InfoType_AslrRegionSize);
    g.address_bits = code_end <= (1ull << 32) ? 32 : code_end <= (1ull << 36) ? 36 : 39;

    g.process = envGetOwnProcessHandle();
    const bool map_code = envIsSyscallHinted(0x77) && envIsSyscallHinted(0x78);
    const bool set_perm = envIsSyscallHinted(0x73);
    g.can_map = g.process != INVALID_HANDLE && map_code && set_perm;
    nx_log("guest memory: %d-bit address space; code region %#llx-%#llx, heap region %#llx, alias region %#llx\n",
           g.address_bits, (unsigned long long)code, (unsigned long long)code_end,
           (unsigned long long)info(InfoType_HeapRegionAddress), (unsigned long long)info(InfoType_AliasRegionAddress));
    nx_log("guest memory: process handle %s, svcMapProcessCodeMemory %s, svcSetProcessMemoryPermission %s\n",
           g.process != INVALID_HANDLE ? "given" : "missing", map_code ? "allowed" : "not allowed",
           set_perm ? "allowed" : "not allowed");
    if (!g.can_map) {
        g.open_failed = "getting the process handle and syscalls";
        nx_log("guest memory: failed at %s\n", g.open_failed);
        return NULL;
    }

    virtmemLock();
    const uint64_t window = find_window();
    g.reservation = window ? virtmemAddReservation((void *)window, NX_GUEST_SIZE) : NULL;
    virtmemUnlock();
    if (!g.reservation) {
        g.open_failed = "finding a free 4 GB window";
        nx_log("guest memory: failed at %s\n", g.open_failed);
        return NULL;
    }

    g.page_bitmap = page_bitmap;
    g.open_failed = NULL;
    g.window = (uint8_t *)window;
    nx_log("guest memory: window at %#llx\n", (unsigned long long)window);
    return g.window;
}

uint32_t nx_guest_commit(uint64_t guest, uint64_t size)
{
    uint64_t first = guest / PIECE, end = (guest + size + PIECE - 1) / PIECE;
    if (end > PIECES)
        end = PIECES;
    if (!g.window || size == 0 || first >= end)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    mutexLock(&g.lock);
    Result rc = 0;
    for (uint64_t p = first; p < end && R_SUCCEEDED(rc);) {
        if (is_mapped(p)) {
            p++;
            continue;
        }
        uint64_t q = p;
        while (q < end && !is_mapped(q))
            q++;
        rc = map_run(p, q - p);
        if (R_FAILED(rc))
            note_refusal(rc, p * PIECE);
        p = q;
    }
    mutexUnlock(&g.lock);
    return rc;
}

/* Called by nx_guest_fault.s on the faulting thread, with the kernel's exception info:
 * x0..x8, lr, sp, pc, pstate, afsr0, afsr1, esr, far. The kernel lets one thread in at a time.
 * Does not log: the faulting thread may hold the log's lock, or stdio's. */
int nx_guest_fault(uint32_t type, const uint8_t *exception)
{
    (void)type;
    uint32_t esr;
    uint64_t far;
    memcpy(&esr, exception + 0x6C, sizeof esr);
    memcpy(&far, exception + 0x70, sizeof far);

    /* a data abort from the program (EC 0x24) on an unmapped page (a translation fault),
     * with a valid fault address (FnV clear), inside the window */
    const uint64_t window = (uint64_t)g.window;
    if (!window || (esr >> 26) != 0x24 || (esr & (1u << 10)) || (esr & 0x3C) != 0x04)
        return -1;
    if (far < window || far - window >= NX_GUEST_SIZE)
        return -1;

    const uint64_t piece = (far - window) / PIECE;
    mutexLock(&g.lock);
    Result rc = 0;
    if (!is_mapped(piece)) {        /* or another thread faulted on it and mapped it first */
        rc = map_run(piece, 1);
        if (R_SUCCEEDED(rc))
            g.faults++;
        else
            note_refusal(rc, piece * PIECE);
    }
    mutexUnlock(&g.lock);
    return R_SUCCEEDED(rc) ? 0 : -1;
}

uint64_t nx_guest_window(void)
{
    return (uint64_t)g.window;
}

void nx_guest_stats(NxGuestStats *out)
{
    mutexLock(&g.lock);
    *out = (NxGuestStats){
        .window = (uint64_t)g.window,
        .mapped = g.mapped_bytes,
        .maps = g.map_count,
        .faults = g.faults,
        .refused = g.refused,
        .refused_rc = g.refused_rc,
        .refused_at = g.refused_at,
    };
    mutexUnlock(&g.lock);
}

void nx_guest_close(void)
{
    if (!g.window)
        return;
    mutexLock(&g.lock);
    uint8_t *window = g.window;
    g.window = NULL;    /* faults are no longer ours */
    const unsigned maps = g.map_count;
    const uint64_t mapped = g.mapped_bytes;

    Result first = 0;
    unsigned kept = 0;
    while (g.map_count) {
        const Mapping *m = &g.maps[--g.map_count];
        Result rc = svcUnmapProcessCodeMemory(g.process, (uint64_t)window + m->guest, m->src, m->size);
        if (R_SUCCEEDED(rc)) {
            free((void *)m->src);
        } else {
            kept++;     /* still mapped: it must not go back to malloc */
            if (R_SUCCEEDED(first))
                first = rc;
        }
    }
    memset(g.mapped, 0, sizeof g.mapped);
    if (g.page_bitmap)
        memset(g.page_bitmap, 0, GUEST_ALIGN / PAGE_64K / 8);
    g.mapped_bytes = 0;
    mutexUnlock(&g.lock);

    virtmemLock();
    virtmemRemoveReservation(g.reservation);
    virtmemUnlock();
    g.reservation = NULL;

    if (kept)
        nx_log("guest memory: %u of %u mappings could not be unmapped (result %#x): restart the game before the next NRO\n",
               kept, maps, first);
    else
        nx_log("guest memory: closed, %u mappings and %llu MB given back\n", maps, (unsigned long long)(mapped >> 20));
}

/* ---- the test NRO's check ---------------------------------------------------------------- */

/* what the probe commits as large ranges: main memory, above the first piece, and the RSX's */
#define MAIN_BASE    0x00100000ull
#define MAIN_SIZE    0x0FF00000ull
#define RSX_BASE     0xC0000000ull
#define RSX_SIZE     0x10000000ull

/* then pieces one at a time, one after the other, as a run of faults maps them */
#define RUN_BASE     0x20000000ull
#define RUN_PIECES   64u

/* guest addresses nothing has mapped: the null page, the gaps, the stacks, the top edge */
static const uint64_t untouched[] = {
    0x00000000, 0x10000000, 0x30000000, 0x40000000, 0x7FFF0000,
    0xD0000000, 0xE0000000, 0xFFFFFFFC, 0x100000000,
};
#define UNTOUCHED (sizeof untouched / sizeof untouched[0])

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

/* The three ways the game maps memory. Returns the step that failed, or NULL. */
static const char *run(NxGuestProbe *out, Result *rc)
{
    uint64_t start = armGetSystemTick();
    if ((*rc = nx_guest_commit(MAIN_BASE, MAIN_SIZE)) || (*rc = nx_guest_commit(RSX_BASE, RSX_SIZE)))
        return "mapping main and RSX memory";
    out->map_ms = (uint32_t)(ns_since(start) / 1000000);
    if (!check_word(MAIN_BASE) || !check_word(MAIN_BASE + MAIN_SIZE - 4) ||
        !check_word(RSX_BASE) || !check_word(RSX_BASE + RSX_SIZE - 4))
        return "reading back main and RSX memory";
    nx_log("guest probe: main and RSX memory, 511 MB, committed and zeroed in %u ms\n", out->map_ms);

    start = armGetSystemTick();
    for (unsigned i = 0; i < RUN_PIECES; i++)
        if ((*rc = nx_guest_commit(RUN_BASE + i * PIECE, 4)))
            return "committing pieces";
    out->piece_us = (uint32_t)(ns_since(start) / 1000 / RUN_PIECES);
    if (!check_word(RUN_BASE) || !check_word(RUN_BASE + RUN_PIECES * PIECE - 4))
        return "reading back the pieces";
    MemoryInfo mi;
    u32 page;
    const uint64_t run_at = (uint64_t)g.window + RUN_BASE;
    out->pieces_merge = R_SUCCEEDED(svcQueryMemory(&mi, &page, run_at)) && mi.addr <= run_at &&
                        mi.addr + mi.size >= run_at + RUN_PIECES * PIECE;
    nx_log("guest probe: %u pieces of 1 MB at %u us each, %s\n", RUN_PIECES, out->piece_us,
           out->pieces_merge ? "one block for the kernel" : "kept apart by the kernel");

    nx_log("guest probe: touching unmapped guest memory; if the log ends here, faults do not reach the NRO\n");
    const unsigned faults_before = g.faults;
    start = armGetSystemTick();
    for (unsigned i = 0; i < UNTOUCHED; i++)
        if (!check_word(untouched[i]))
            return "reading memory mapped on a fault";
    __asm__ volatile("" ::: "memory");      /* g.faults changed under the loads above */
    const uint64_t ns = ns_since(start);
    out->faults = g.faults - faults_before;
    if (out->faults != UNTOUCHED) {
        *rc = g.refused_rc;
        return "answering faults";
    }
    out->fault_us = (uint32_t)(ns / 1000 / UNTOUCHED);
    nx_log("guest probe: %u faults answered at %u us each\n", out->faults, out->fault_us);

    NxGuestStats stats;
    nx_guest_stats(&stats);
    nx_log("guest probe: %u mappings, %llu MB\n", stats.maps, (unsigned long long)(stats.mapped >> 20));
    return NULL;
}

void nx_guest_probe(NxGuestProbe *out)
{
    memset(out, 0, sizeof *out);
    uint8_t *window = nx_guest_open(NULL);
    out->address_bits = g.address_bits;
    out->can_map = g.can_map;
    if (!window) {
        out->failed = g.open_failed;
        return;
    }
    out->window = (uint64_t)window;

    Result rc = 0;
    out->failed = run(out, &rc);
    nx_guest_close();
    out->rc = rc;
    if (out->failed)
        nx_log("guest probe: failed at %s, result %#x\n", out->failed, rc);
    else
        nx_log("guest probe: works without svcMapPhysicalMemory\n");
}
