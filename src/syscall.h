#pragma once
#include "riscv.h"

/* Microkernel system calls. Number in a7; args in a0..a2; return in a0.
   Every syscall traps via `ecall` into _mtrap, so the caller's full context
   is saved before the kernel may block it or switch tasks. */
enum {
    SYS_YIELD = 0,
    SYS_SEND  = 1,   /* a0 = dest task id, a1 = message word */
    SYS_RECV  = 2,   /* returns sender id in a0, message in a1 (via *out) */
    SYS_PUTC  = 3,   /* a0 = char (kernel-mediated console) */
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

static inline void sys_yield(void)              { _ecall1(SYS_YIELD, 0); }
static inline void sys_send(int dst, uint64 m)  { _ecall2(SYS_SEND, dst, (long)m); }
/* recv: blocks until a message arrives; kernel writes *msg and returns the
   sender's task id in a0. */
static inline int sys_recv(uint64 *msg)         { return (int)_ecall1(SYS_RECV, (long)msg); }
