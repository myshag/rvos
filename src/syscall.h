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
    /* Kernel state a user-mode server cannot read for itself. The task table,
       the mount tables and the page allocator all live in kernel memory, so
       the proc server asks instead of looking. */
    SYS_TASKINFO = 6,  /* a0 = index, a1 = struct taskinfo* -> 0 / -1 */
    SYS_MOUNTS   = 7,  /* a0 = task id, a1 = buf, a2 = cap -> bytes */
    SYS_MEMINFO  = 8,  /* a0 = int[2] out: {free pages, total pages} */
    /* Namespace mutation: also kernel-side per-task state. */
    SYS_BIND     = 9,  /* a0 = prefix, a1 = server task id */
    SYS_NSCLONE  = 10,
    /* Loading a program. Three primitives, matched to what an ELF program
       header actually says, so the loader can live in user space and the
       kernel need never learn what ELF is:
         NEWTASK  make an empty address space              -> task id
         VMLOAD   turn one PT_LOAD segment into pages
         START    set the entry point and let it run
       They are deliberately unguarded — any task may build another. Real
       systems put a capability in front of exactly these. */
    SYS_NEWTASK = 11,  /* a0 = name -> task id, or -1 */
    SYS_VMLOAD  = 12,  /* a0 = task id, a1 = struct vmload* -> 0 / -1 */
    SYS_START   = 13,  /* a0 = task id, a1 = struct startinfo* -> 0 / -1 */
    SYS_EXIT    = 14,  /* never returns; frees this task's memory */
    /* Interrupt handling for user-mode drivers. The kernel is the only thing
       that can take a trap, so it claims the interrupt, leaves the source
       masked and tells the owner; the driver touches the device itself and
       acks, which unmasks. The kernel never learns what the device is. */
    SYS_IRQ_REG = 15,  /* a0 = irq — this task becomes its driver */
    SYS_IRQ_ACK = 16,  /* a0 = irq — handled; let it fire again */
};

/* sys_recv() returns this instead of a task id when what arrived was an
   interrupt rather than a message. A driver's receive loop then handles both
   without needing a second blocking primitive. */
#define IRQ_SENDER (-2)

struct taskinfo {
    int  id;
    int  state;          /* matches enum task_state */
    int  is_current;
    char name[16];
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

static inline int sys_taskinfo(int idx, struct taskinfo *out)
{
    return (int)_ecall2(SYS_TASKINFO, idx, (long)out);
}
static inline int sys_mounts(int task_id, char *out, int cap)
{
    return (int)_ecall3(SYS_MOUNTS, task_id, (long)out, cap);
}
static inline int sys_meminfo(int out[2])
{
    return (int)_ecall1(SYS_MEMINFO, (long)out);
}
static inline int sys_bind(const char *prefix, int server_task)
{
    return (int)_ecall2(SYS_BIND, (long)prefix, server_task);
}
static inline int sys_nsclone(void)
{
    return (int)_ecall1(SYS_NSCLONE, 0);
}

static inline int sys_newtask(const char *name)
{
    return (int)_ecall1(SYS_NEWTASK, (long)name);
}
static inline int sys_vmload(int tid, const void *seg)
{
    return (int)_ecall2(SYS_VMLOAD, tid, (long)seg);
}
static inline int sys_start(int tid, const void *startinfo)
{
    return (int)_ecall2(SYS_START, tid, (long)startinfo);
}

static inline int sys_irq_register(int irq)
{
    return (int)_ecall1(SYS_IRQ_REG, irq);
}
static inline int sys_irq_ack(int irq)
{
    return (int)_ecall1(SYS_IRQ_ACK, irq);
}

static inline void sys_exit(void)
{
    _ecall1(SYS_EXIT, 0);
    for (;;) ;                 /* not reached */
}

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
