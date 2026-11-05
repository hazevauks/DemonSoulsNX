/* hos_compat.h -- what ps3recomp's runtime expects of a POSIX host that Horizon's C library
 * leaves out, or keeps behind feature macros.
 *
 * Force-included into every source of the runtime (CMakeLists.txt), so the vendored files build
 * unchanged. The shim headers beside it (include/) stand in for the ones Horizon does not have.
 */
#pragma once

#ifdef __SWITCH__

/* glibc's netdb.h brings these in; newlib's does not, and the network modules rely on it */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <pthread.h>
#include <time.h>

/* newlib's limits.h has none; libnx takes any stack of whole pages */
#ifndef PTHREAD_STACK_MIN
#define PTHREAD_STACK_MIN 0x4000
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* not in newlib */
time_t timegm(struct tm *tm);

/* declared by newlib only under _POSIX_TIMEOUTS, and not implemented by libnx */
int hos_pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline);
#define pthread_mutex_timedlock hos_pthread_mutex_timedlock

#ifdef __cplusplus
}
#endif

#endif /* __SWITCH__ */
