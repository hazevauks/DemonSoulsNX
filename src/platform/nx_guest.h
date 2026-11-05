/* nx_guest.h -- the PS3's 4 GB address space on the Switch. */

#ifndef NX_GUEST_H
#define NX_GUEST_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int address_bits;           /* the process's address space: 32, 36 or 39 bits */
    bool can_map;               /* hbloader gave the process handle and the syscalls it needs */
    uint64_t window;            /* host address of guest address 0, on a 4 GB boundary; 0 = none */
    uint32_t map_ms;            /* time to map main and RSX memory, 512 MB */
    uint32_t piece_us;          /* time to map one 64 KB piece */
    bool pieces_merge;          /* pieces mapped one after the other became one block */
    unsigned faults;            /* faults in the window answered by mapping memory */
    uint32_t fault_us;          /* time per fault, mapping included */
    const char *failed;         /* the step that failed, or NULL when all of it works */
    uint32_t rc;                /* that step's result code */
} NxGuestProbe;

/* Stage 1: maps guest memory the way the runtime will, checks it, and gives it all back. */
void nx_guest_probe(NxGuestProbe *out);

/* Called by nx_guest_fault.s with the kernel's exception info; 0 = the access can run again. */
int nx_guest_fault(uint32_t type, const uint8_t *info);

#ifdef __cplusplus
}
#endif

#endif /* NX_GUEST_H */
