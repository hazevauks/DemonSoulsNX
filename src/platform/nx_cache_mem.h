/* nx_cache_mem.h -- the game's cache (/dev_hdd1) in memory, not on the SD card. */

#ifndef NX_CACHE_MEM_H
#define NX_CACHE_MEM_H

#include <stdbool.h>

/* Up to DESNX_CACHE_MEMORY_MB (settings.txt; 0 leaves the cache on the SD card) as a drive of its
 * own, NX_HDD1_MEM_DIR, and the runtime's /dev_hdd1 on it. Call after the settings are read and
 * before the boot. False when the cache stays on the SD card. */
bool nx_cache_mem_init(void);

/* Logs what the cache holds, when that changed since the last call. */
void nx_cache_mem_report(void);

#endif /* NX_CACHE_MEM_H */
