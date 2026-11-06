/* nx_guest_fault.s -- where the kernel sends a thread that faults.
 *
 * On a fault, Horizon runs the process's entry point again on the faulting thread, with
 * x0 = the kind of exception and x1 = the kernel's copy of x0..x8, lr, sp, pc, pstate, esr and
 * far. hbloader passes that on to the NRO's entry, and libnx's start code branches here. This
 * takes the place of libnx's own entry, which can only report a crash: nx_exception (nx_crash.c)
 * has nx_guest_fault map memory when the fault is in the guest window, and the faulting access
 * runs again; any other fault it writes up in SD_LOG before the kernel ends the process.
 *
 * svcReturnFromException(0) puts back x0..x8, lr, sp, pc and pstate from the kernel's copy.
 * x9..x29, the NEON registers, FPCR and FPSR still hold the faulting thread's values, so they
 * are saved around the C call: x9..x18 because the C code may use them, x19..x29 for the crash
 * report (the C code keeps them itself; x29 is the frame pointer a backtrace starts from). The
 * save area's address goes to nx_exception as its third argument.
 */

    .section .text.__libnx_exception_entry, "ax", %progbits
    .global __libnx_exception_entry
    .type   __libnx_exception_entry, %function
    .align  2
__libnx_exception_entry:
    adrp    x2, fault_stack_top
    add     x2, x2, :lo12:fault_stack_top
    mov     sp, x2
    sub     sp, sp, #0x2c0

    stp     x9, x10, [sp, #0x000]
    stp     x11, x12, [sp, #0x010]
    stp     x13, x14, [sp, #0x020]
    stp     x15, x16, [sp, #0x030]
    stp     x17, x18, [sp, #0x040]
    mrs     x2, fpcr
    mrs     x3, fpsr
    stp     x2, x3, [sp, #0x050]
    stp     q0, q1, [sp, #0x060]
    stp     q2, q3, [sp, #0x080]
    stp     q4, q5, [sp, #0x0a0]
    stp     q6, q7, [sp, #0x0c0]
    stp     q8, q9, [sp, #0x0e0]
    stp     q10, q11, [sp, #0x100]
    stp     q12, q13, [sp, #0x120]
    stp     q14, q15, [sp, #0x140]
    stp     q16, q17, [sp, #0x160]
    stp     q18, q19, [sp, #0x180]
    stp     q20, q21, [sp, #0x1a0]
    stp     q22, q23, [sp, #0x1c0]
    stp     q24, q25, [sp, #0x1e0]
    stp     q26, q27, [sp, #0x200]
    stp     q28, q29, [sp, #0x220]
    stp     q30, q31, [sp, #0x240]
    add     x3, sp, #0x260              /* past stp's reach from sp */
    stp     x19, x20, [x3, #0x00]
    stp     x21, x22, [x3, #0x10]
    stp     x23, x24, [x3, #0x20]
    stp     x25, x26, [x3, #0x30]
    stp     x27, x28, [x3, #0x40]
    str     x29, [x3, #0x50]

    mov     x2, sp
    bl      nx_exception                /* w0 = 0: the access can run again */

    ldp     x2, x3, [sp, #0x050]
    msr     fpcr, x2
    msr     fpsr, x3
    ldp     x9, x10, [sp, #0x000]
    ldp     x11, x12, [sp, #0x010]
    ldp     x13, x14, [sp, #0x020]
    ldp     x15, x16, [sp, #0x030]
    ldp     x17, x18, [sp, #0x040]
    ldp     q0, q1, [sp, #0x060]
    ldp     q2, q3, [sp, #0x080]
    ldp     q4, q5, [sp, #0x0a0]
    ldp     q6, q7, [sp, #0x0c0]
    ldp     q8, q9, [sp, #0x0e0]
    ldp     q10, q11, [sp, #0x100]
    ldp     q12, q13, [sp, #0x120]
    ldp     q14, q15, [sp, #0x140]
    ldp     q16, q17, [sp, #0x160]
    ldp     q18, q19, [sp, #0x180]
    ldp     q20, q21, [sp, #0x1a0]
    ldp     q22, q23, [sp, #0x1c0]
    ldp     q24, q25, [sp, #0x1e0]
    ldp     q26, q27, [sp, #0x200]
    ldp     q28, q29, [sp, #0x220]
    ldp     q30, q31, [sp, #0x240]

    cbz     w0, 1f
    mov     w0, #0xf801                 /* not ours: the kernel ends the process, as libnx's would */
1:  bl      svcReturnFromException
    b       .
    .size   __libnx_exception_entry, . - __libnx_exception_entry

/* The kernel lets one thread at a time handle an exception, so one stack is enough: the crash
 * report formats on it too. */
    .section .bss.nx_guest_fault_stack, "aw", %nobits
    .align  4
fault_stack:
    .space  0x8000
fault_stack_top:
