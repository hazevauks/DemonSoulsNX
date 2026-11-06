/* nx_crash.h -- the crash report in SD_LOG (nx_crash.c). */

#ifndef NX_CRASH_H
#define NX_CRASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version for the report's first line, and a hook that writes what the PS3 code was doing
 * on the faulting thread (the game's; NULL for none). */
void nx_crash_init(const char *version, size_t (*context)(char *out, size_t size));

/* From nx_guest_fault.s: guest memory first, else the report. 0 = the access can run again. */
int nx_exception(uint32_t type, const uint8_t *info, const uint64_t *saved);

/* A report from where it is called (abort, a failed assert). */
void nx_crash_report_here(const char *why);

/* Whether [address, address + size) is mapped and readable: for a context hook, which must not
 * fault inside the report. */
bool nx_crash_readable(uint64_t address, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* NX_CRASH_H */
