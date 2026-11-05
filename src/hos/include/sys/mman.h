/* sys/mman.h -- memory mappings on Horizon, for ps3recomp's runtime.
 *
 * Horizon has no mmap. src/hos/hos_compat.c gives the runtime what it asks of it outside guest
 * memory: anonymous memory (from the heap, zeroed) and read-only views of files (read into
 * memory). It cannot reserve address space or protect pages: the guest's 4 GB and its pages go
 * through src/platform/nx_guest.c instead (stage 6). A reservation fails with ENOMEM, a fixed
 * address with ENOSYS, and mprotect is accepted on the memory it handed out but not enforced.
 */
#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4

#define MAP_SHARED      0x01
#define MAP_PRIVATE     0x02
#define MAP_FIXED       0x10
#define MAP_ANONYMOUS   0x20
#define MAP_ANON        MAP_ANONYMOUS
#define MAP_NORESERVE   0x4000

#define MAP_FAILED      ((void *)-1)

#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4

#define MS_ASYNC        1
#define MS_INVALIDATE   2
#define MS_SYNC         4

void *mmap(void *address, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int prot);
int madvise(void *address, size_t length, int advice);
int msync(void *address, size_t length, int flags);

#ifdef __cplusplus
}
#endif
