/* nx_system.c -- what the console gives the port: firmware, launch mode, memory.
 *
 * The PS3 code runs in a 32-bit guest address space. The first plan was to map it into the
 * alias region with svcMapPhysicalMemory, which needs a system resource budget; the first
 * console test showed hbloader's process has none, and nx_guest.c maps it another way. The
 * figures are still logged: a different loader may give them.
 */

#include <switch.h>

#include "nx_log.h"
#include "nx_system.h"

static uint64_t info(InfoType type)
{
    uint64_t value = 0;
    if (R_FAILED(svcGetInfo(&value, type, CUR_PROCESS_HANDLE, 0)))
        return 0;
    return value;
}

void nx_system_read(NxSystem *out)
{
    AppletType type = appletGetAppletType();

    out->firmware = hosversionGet();
    out->atmosphere = hosversionIsAtmosphere();
    out->application = type == AppletType_Application || type == AppletType_SystemApplication;
    out->memory_total = info(InfoType_TotalMemorySize);
    out->memory_used = info(InfoType_UsedMemorySize);
    out->alias_region = info(InfoType_AliasRegionSize);
    out->system_resource = info(InfoType_SystemResourceSizeTotal);
    out->map_physical_memory = envIsSyscallHinted(0x2C) && out->system_resource != 0;
}

void nx_system_log(const NxSystem *s)
{
    nx_log("system: firmware %u.%u.%u%s, %s\n",
           HOSVER_MAJOR(s->firmware), HOSVER_MINOR(s->firmware), HOSVER_MICRO(s->firmware),
           s->atmosphere ? " (Atmosphere)" : "",
           s->application ? "application (full memory)" : "applet (reduced memory)");
    nx_log("memory: %lu MB for the process, %lu MB in use\n",
           (unsigned long)(s->memory_total >> 20), (unsigned long)(s->memory_used >> 20));
    nx_log("guest memory: alias region %lu GB, system resource %lu MB, svcMapPhysicalMemory %s\n",
           (unsigned long)(s->alias_region >> 30), (unsigned long)(s->system_resource >> 20),
           s->map_physical_memory ? "allowed" : "not allowed");
}
