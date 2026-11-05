/* ucontext.h -- no getcontext/swapcontext on Horizon yet.
 *
 * Only cellFiber (libs/spurs/cellFiber.c) uses them, and Demon's Souls does not use cellFiber.
 * The types let it build; every call fails with ENOSYS, so a title that does use fibers says so.
 */
#pragma once

#include <signal.h>     /* stack_t */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t regs[31];
    uint64_t sp, pc, pstate;
    uint8_t fpregs[32 * 16];
} mcontext_t;

typedef struct ucontext_t {
    struct ucontext_t *uc_link;
    stack_t uc_stack;
    sigset_t uc_sigmask;
    mcontext_t uc_mcontext;
} ucontext_t;

int getcontext(ucontext_t *context);
int setcontext(const ucontext_t *context);
int swapcontext(ucontext_t *from, const ucontext_t *to);
void makecontext(ucontext_t *context, void (*fn)(void), int argc, ...);

#ifdef __cplusplus
}
#endif
