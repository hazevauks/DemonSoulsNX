/* nx_system.h -- what the console gives the port: firmware, launch mode, memory. */

#ifndef NX_SYSTEM_H
#define NX_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t firmware;              /* MAKEHOSVERSION form */
    bool atmosphere;
    bool application;               /* started as an application (title takeover), not as an applet */
    uint64_t memory_total;          /* bytes the process may use */
    uint64_t memory_used;
    uint64_t alias_region;          /* size of the region svcMapPhysicalMemory maps into */
    uint64_t system_resource;       /* page-table budget svcMapPhysicalMemory needs; 0 = none */
    bool map_physical_memory;       /* svcMapPhysicalMemory is allowed */
} NxSystem;

void nx_system_read(NxSystem *out);
void nx_system_log(const NxSystem *info);

#ifdef __cplusplus
}
#endif

#endif /* NX_SYSTEM_H */
