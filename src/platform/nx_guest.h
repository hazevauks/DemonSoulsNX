/* nx_guest.h -- the PS3's 4 GB address space on the Switch. */

#ifndef NX_GUEST_H
#define NX_GUEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NX_GUEST_SIZE   0x100100000ull  /* 4 GB and a piece above it, for reads past the top */
#define NX_GUEST_PIECE  0x100000ull     /* what a fault maps, and what a commit is rounded out to */

typedef struct {
    uint64_t window;            /* host address of guest address 0, or 0 while it is closed */
    uint64_t mapped;            /* bytes of heap memory mapped into it */
    unsigned maps;              /* separate mappings: each is a memory block of its own for the kernel */
    unsigned faults;            /* touches of unmapped memory answered with a piece */
    unsigned refused;           /* commits and faults that could not map */
    uint32_t refused_rc;        /* the first refusal's result code */
    uint64_t refused_at;        /* and its guest address */
} NxGuestStats;

/* Finds a free 4 GB window on a 4 GB boundary and takes it for guest memory: from then on a
 * fault there maps a piece of heap memory, which reads as zero. Returns the window (vm_base),
 * or NULL with the reason logged. `page_bitmap`, if given, is kept in step, a bit for each
 * 64 KB of guest memory mapped (the runtime's g_vm_page_bitmap, which its SPU DMA checks). */
uint8_t *nx_guest_open(uint8_t *page_bitmap);

/* Maps guest memory over [guest, guest + size), rounded out to whole pieces, where it is not
 * mapped yet, in as few mappings as it can. 0, or the result code of the step that failed. */
uint32_t nx_guest_commit(uint64_t guest, uint64_t size);

void nx_guest_stats(NxGuestStats *out);

/* The window's host address, or 0; takes no lock, for the crash report. */
uint64_t nx_guest_window(void);

/* Unmaps it all and gives the memory back to the heap: the process returns to hbloader, which
 * keeps running in it. Guest memory touched after this is a crash. */
void nx_guest_close(void);

typedef struct {
    int address_bits;           /* the process's address space: 32, 36 or 39 bits */
    bool can_map;               /* hbloader gave the process handle and the syscalls it needs */
    uint64_t window;            /* where the window was, on a 4 GB boundary; 0 = none */
    uint32_t map_ms;            /* time to commit main and RSX memory, 512 MB */
    uint32_t piece_us;          /* time to commit one piece */
    bool pieces_merge;          /* pieces mapped one after the other became one block */
    unsigned faults;            /* faults in the window answered by mapping memory */
    uint32_t fault_us;          /* time per fault, mapping included */
    const char *failed;         /* the step that failed, or NULL when all of it works */
    uint32_t rc;                /* that step's result code */
} NxGuestProbe;

/* The test NRO: opens guest memory, maps it the three ways the game does (a commit of a large
 * range, a run of pieces, faults), checks it, and closes it again. */
void nx_guest_probe(NxGuestProbe *out);

/* Called by nx_guest_fault.s with the kernel's exception info; 0 = the access can run again. */
int nx_guest_fault(uint32_t type, const uint8_t *info);

#ifdef __cplusplus
}
#endif

#endif /* NX_GUEST_H */
