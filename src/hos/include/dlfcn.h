/* dlfcn.h -- Horizon loads no shared objects: the whole port is one NRO.
 *
 * The runtime uses dladdr to name a crash's frames and dlopen only for the split build that loads
 * the game from a shared object, which the Switch does not use. These answer "nothing loaded".
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   0x1
#define RTLD_NOW    0x2
#define RTLD_LOCAL  0x0
#define RTLD_GLOBAL 0x100

typedef struct {
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;

int dladdr(const void *address, Dl_info *info);
void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);

#ifdef __cplusplus
}
#endif
