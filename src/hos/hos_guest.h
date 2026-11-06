/* hos_guest.h -- how the shim's mprotect reaches the guest's 4 GB (src/platform/nx_guest.c).
 *
 * Kept out of hos_compat.h on purpose: that header is forced into every lifted chunk, and ninja
 * compiles all of them again whenever it changes.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* From then on, mprotect making memory in [window, window + size) readable or writable calls
 * `commit` with the guest address and length, and fails if it does not return 0. */
void hos_guest_window(void *window, uint64_t size, uint32_t (*commit)(uint64_t guest, uint64_t size));

#ifdef __cplusplus
}
#endif
