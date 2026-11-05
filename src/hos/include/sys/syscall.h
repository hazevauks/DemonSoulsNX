/* sys/syscall.h -- Horizon has no Linux system call numbers.
 *
 * ps3recomp's POSIX layer includes this header on every POSIX host and calls syscall() only on
 * Linux (GetCurrentThreadId, a thread's nice value). Nothing here: the header only has to exist.
 */
#pragma once
