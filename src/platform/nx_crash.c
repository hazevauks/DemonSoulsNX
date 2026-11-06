/* nx_crash.c -- what a crash leaves in SD_LOG/demonsouls_crash.txt.
 *
 * Every fault reaches nx_exception, from the process's exception entry (nx_guest_fault.s). One in
 * the guest window maps memory (nx_guest.c) and the access runs again; any other is a crash, which
 * the kernel ends the process for. Before it does, this writes down what happened: the kind of
 * fault from the kernel's ESR, the address, the registers, the call stack by its frame pointers,
 * and, through a hook the game sets, what the PS3 code was doing. Addresses in the NRO are given
 * as offsets, which are its ELF's addresses: tools/crash_symbolize.py names them on the PC.
 *
 * It runs on the faulting thread, which may hold any lock: the log's, stdio's, malloc's. So it
 * formats into a buffer of its own, with integers only (newlib's printf takes no lock for a
 * string, but allocates for floating point), and writes through the file system service, not
 * through stdio. Nothing it reads is trusted: a pointer is checked as mapped before it is read.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <switch.h>

#include "nx_crash.h"
#include "nx_guest.h"

#define CRASH_PATH  "/switch/demonsouls/SD_LOG/demonsouls_crash.txt"
#define FRAMES      32

extern void _start(void);       /* the NRO's first byte: its ELF's address 0 */
extern char __end__[];          /* past its .bss */

static char s_buf[24 * 1024];
static size_t s_len;
static const char *s_version = "?";
static size_t (*s_context)(char *out, size_t size);
static uint32_t s_taken;

void nx_crash_init(const char *version, size_t (*context)(char *out, size_t size))
{
    s_version = version;
    s_context = context;
}

static void put(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void put(const char *format, ...)
{
    if (s_len >= sizeof s_buf - 1)
        return;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(s_buf + s_len, sizeof s_buf - s_len, format, args);
    va_end(args);
    if (n > 0)
        s_len += (size_t)n < sizeof s_buf - s_len ? (size_t)n : sizeof s_buf - s_len - 1;
}

bool nx_crash_readable(uint64_t address, size_t size)
{
    MemoryInfo mi;
    u32 page;
    if (R_FAILED(svcQueryMemory(&mi, &page, address)))
        return false;
    return mi.type != MemType_Unmapped && (mi.perm & Perm_R) && address + size <= mi.addr + mi.size;
}

/* An address, and where it is: the NRO (its ELF's address), or the guest's memory. */
static void put_where(uint64_t a)
{
    const uint64_t base = (uint64_t)&_start, end = (uint64_t)__end__;
    const uint64_t window = nx_guest_window();
    if (a >= base && a < end)
        put(" (nro+0x%llx)", (unsigned long long)(a - base));
    else if (window && a >= window && a - window < NX_GUEST_SIZE)
        put(" (guest 0x%08llx)", (unsigned long long)(a - window));
}

static void put_reg(const char *name, uint64_t value)
{
    put("  %-4s 0x%016llx", name, (unsigned long long)value);
    put_where(value);
    put("\n");
}

static const char *exception_name(uint32_t esr)
{
    switch (esr >> 26) {
    case 0x00: return "an unknown instruction";
    case 0x07: return "a floating-point or vector instruction, not allowed";
    case 0x0E: return "an illegal execution state";
    case 0x15: return "a system call";
    case 0x18: return "a system register access";
    case 0x20: case 0x21: return "an instruction fetch fault";
    case 0x22: return "a misaligned PC";
    case 0x24: case 0x25: return "a data access fault";
    case 0x26: return "a misaligned stack pointer";
    case 0x2C: return "a floating-point exception";
    case 0x30: case 0x31: return "a breakpoint";
    case 0x32: case 0x33: return "a software step";
    case 0x34: case 0x35: return "a watchpoint";
    case 0x3C: return "a BRK instruction (a trap the code set: __builtin_trap, an assert)";
    default: return "an exception of another kind";
    }
}

static const char *fault_name(uint32_t fsc)
{
    if (fsc <= 0x03) return "address size fault";
    if (fsc <= 0x07) return "translation fault: nothing is mapped there";
    if (fsc >= 0x09 && fsc <= 0x0B) return "access flag fault";
    if (fsc >= 0x0D && fsc <= 0x0F) return "permission fault: mapped, but not for this access";
    if (fsc == 0x10) return "synchronous external abort";
    if (fsc == 0x21) return "alignment fault: an exclusive or atomic access off its size's boundary";
    if (fsc == 0x30) return "TLB conflict";
    return "fault of another kind";
}

static const char *desc_name(uint32_t type)
{
    switch (type) {
    case 0x100: return "instruction abort";
    case 0x101: return "other";
    case 0x102: return "misaligned PC";
    case 0x103: return "misaligned SP";
    case 0x104: return "trap";
    case 0x106: return "SError";
    case 0x301: return "bad SVC";
    default: return "?";
    }
}

/* The frame records x29 points at: {the caller's x29, the return address}, up the stack. A
 * fault's report starts from its pc and lr; one from where it is called (lr 0), from its pc. */
static void put_backtrace(uint64_t pc, uint64_t lr, uint64_t fp)
{
    put("\nthe call stack, by frame pointers (the innermost first):\n");
    put("  #0  0x%016llx", (unsigned long long)pc);
    put_where(pc);
    put("\n");
    int i = 1;
    if (lr) {
        put("  #1  0x%016llx", (unsigned long long)lr);
        put_where(lr);
        put(" (the link register)\n");
        i = 2;
    }
    for (; i < FRAMES && fp; i++) {
        if ((fp & 7) || !nx_crash_readable(fp, 16)) {
            put("  (the frame pointer 0x%llx is not readable: the walk ends)\n", (unsigned long long)fp);
            return;
        }
        const uint64_t next = ((const uint64_t *)fp)[0], ret = ((const uint64_t *)fp)[1];
        if (!ret)
            return;
        put("  #%-2d 0x%016llx", i, (unsigned long long)ret);
        put_where(ret);
        put("\n");
        if (next <= fp)
            return;
        fp = next;
    }
}

static void put_tail(void)
{
    u64 tid = 0;
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    put("\nthread %llu\n", (unsigned long long)tid);
    if (s_context) {
        char guest[2048];
        const size_t n = s_context(guest, sizeof guest);
        if (n)
            put("%s", guest);
    }
    put("\nThe NRO is at 0x%llx to 0x%llx: its nro+ offsets are its ELF's addresses, and\n"
        "tools/crash_symbolize.py names them on the PC.\n",
        (unsigned long long)(uint64_t)&_start, (unsigned long long)(uint64_t)__end__);
}

/* Through the file system service, not stdio: stdio's lock may be the faulting thread's. */
static void write_report(void)
{
    svcOutputDebugString(s_buf, s_len);
    FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs)
        return;
    fsFsDeleteFile(fs, CRASH_PATH);
    if (R_FAILED(fsFsCreateFile(fs, CRASH_PATH, (s64)s_len, 0)))
        return;
    FsFile file;
    if (R_FAILED(fsFsOpenFile(fs, CRASH_PATH, FsOpenMode_Write, &file)))
        return;
    fsFileWrite(&file, 0, s_buf, s_len, FsWriteOption_Flush);
    fsFileClose(&file);
}

static bool first(void)
{
    return __atomic_fetch_add(&s_taken, 1, __ATOMIC_ACQ_REL) == 0;
}

int nx_exception(uint32_t type, const uint8_t *info, const uint64_t *saved)
{
    if (nx_guest_fault(type, info) == 0)
        return 0;
    if (!first())
        return -1;  /* the first crash is the one to read; another thread's after it, not */

    const ThreadExceptionFrameA64 *f = (const ThreadExceptionFrameA64 *)info;
    const uint64_t *x19 = saved + 0x260 / 8;    /* nx_guest_fault.s's save area */
    s_len = 0;
    put("Demon's Souls for Nintendo Switch %s: a crash\n\n", s_version);
    put("what: %s (the kernel: %s, ESR 0x%08x)\n", exception_name(f->esr), desc_name(type), f->esr);
    const uint32_t ec = f->esr >> 26;
    if (ec == 0x24 || ec == 0x25 || ec == 0x20 || ec == 0x21) {
        put("      %s", fault_name(f->esr & 0x3F));
        if (ec == 0x24 || ec == 0x25)
            put(", %s", (f->esr & (1u << 6)) ? "writing" : "reading");
        put("\naddress: 0x%016llx", (unsigned long long)f->far);
        put_where(f->far);
        if (f->far < 0x10000)
            put(", near zero: a null pointer");
        put("\n");
    }

    put("\nregisters:\n");
    put_reg("pc", f->elr_el1);
    put_reg("lr", f->lr);
    put_reg("sp", f->sp);
    put_reg("fp", x19[10]);
    char name[8];
    for (int i = 0; i < 9; i++) {
        snprintf(name, sizeof name, "x%d", i);
        put_reg(name, f->cpu_gprs[i]);
    }
    for (int i = 9; i <= 18; i++) {
        snprintf(name, sizeof name, "x%d", i);
        put_reg(name, saved[i - 9]);
    }
    for (int i = 19; i <= 28; i++) {
        snprintf(name, sizeof name, "x%d", i);
        put_reg(name, x19[i - 19]);
    }
    put_backtrace(f->elr_el1, f->lr, x19[10]);
    put_tail();
    write_report();
    return -1;
}

void nx_crash_report_here(const char *why)
{
    if (!first())
        return;
    /* this function's own frame record: {its caller's frame, the return address into it} */
    const uint64_t *own = (const uint64_t *)__builtin_frame_address(0);
    s_len = 0;
    put("Demon's Souls for Nintendo Switch %s: a crash\n\n", s_version);
    put("what: %s\n", why);
    put_backtrace(own[1], 0, own[0]);
    put_tail();
    write_report();
}
