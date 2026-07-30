#pragma once
#include "riscv.h"

/* Microkernel system calls. Number in a7; args in a0..a2; return in a0.
   Every syscall traps via `ecall` into _strap, so the caller's full context
   is saved before the kernel may block it or switch tasks.

   send/recv name a buffer rather than passing a value, because tasks no
   longer share an address space: the kernel translates both sides and copies
   the bytes across. A pointer handed straight to another task would refer to
   whatever happened to live at that address in *its* space. */
enum {
    SYS_YIELD = 0,
    SYS_SEND  = 1,   /* a0 = dest task id, a1 = message address, a2 = length */
    SYS_RECV  = 2,   /* a0 = buffer address, a1 = capacity; -> sender id */
    SYS_PUTC  = 3,   /* a0 = char (kernel-mediated console) */
    /* a0 = task id, a1 = va, a2 = out buffer, a3 = capacity -> bytes written.
       Walking a page table means reading the page-table pages themselves, and
       no task maps those any more — so introspection had to become a kernel
       service the moment address spaces were separated. */
    SYS_PGDUMP = 4,
    /* a0 = path -> server task id. Path resolution reads the caller's mount
       table, which lives in kernel memory; a user-mode task cannot look at it
       directly, so the namespace is reached through the kernel like anything
       else. */
    SYS_ROUTE  = 5,
};

static inline long _ecall1(long n, long a0)
{
    register long x7 __asm__("a7") = n;
    register long x10 __asm__("a0") = a0;
    __asm__ volatile("ecall" : "+r"(x10) : "r"(x7) : "memory");
    return x10;
}
static inline long _ecall2(long n, long a0, long a1)
{
    register long x7 __asm__("a7") = n;
    register long x10 __asm__("a0") = a0;
    register long x11 __asm__("a1") = a1;
    __asm__ volatile("ecall" : "+r"(x10) : "r"(x7), "r"(x11) : "memory");
    return x10;
}
static inline long _ecall3(long n, long a0, long a1, long a2)
{
    register long x7 __asm__("a7") = n;
    register long x10 __asm__("a0") = a0;
    register long x11 __asm__("a1") = a1;
    register long x12 __asm__("a2") = a2;
    __asm__ volatile("ecall" : "+r"(x10) : "r"(x7), "r"(x11), "r"(x12) : "memory");
    return x10;
}

static inline long _ecall4(long n, long a0, long a1, long a2, long a3)
{
    register long x7 __asm__("a7") = n;
    register long x10 __asm__("a0") = a0;
    register long x11 __asm__("a1") = a1;
    register long x12 __asm__("a2") = a2;
    register long x13 __asm__("a3") = a3;
    __asm__ volatile("ecall" : "+r"(x10)
                     : "r"(x7), "r"(x11), "r"(x12), "r"(x13) : "memory");
    return x10;
}

static inline void sys_yield(void) { _ecall1(SYS_YIELD, 0); }

static inline int sys_route(const char *path)
{
    return (int)_ecall1(SYS_ROUTE, (long)path);
}

/* Render one task's translation of `va` as text into our own buffer. */
static inline int sys_pgdump(int task_id, unsigned long va, void *out, int cap)
{
    return (int)_ecall4(SYS_PGDUMP, task_id, (long)va, (long)out, cap);
}

/* Blocks until the destination receives; the kernel copies `len` bytes out
   of this task's address space into the receiver's. */
static inline void sys_send(int dst, const void *msg, int len)
{
    _ecall3(SYS_SEND, dst, (long)msg, len);
}

/* Blocks until a message arrives; returns the sender's task id. */
static inline int sys_recv(void *buf, int len)
{
    return (int)_ecall2(SYS_RECV, (long)buf, len);
}
