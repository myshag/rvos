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
    /* a0 = path, a1 = out, a2 = cap, a3 = which member of the union at that
       name -> server task id, and in `out`
       the name that server should be asked about — not the same name if a
       bind was crossed on the way. Resolution reads the caller's mount table,
       which lives in kernel memory; a user-mode task cannot look at it
       directly, so the namespace is reached through the kernel like anything
       else. */
    SYS_RESOLVE = 5,
    /* Kernel state a user-mode server cannot read for itself. The task table,
       the mount tables and the page allocator all live in kernel memory, so
       the proc server asks instead of looking. */
    SYS_TASKINFO = 6,  /* a0 = index, a1 = struct taskinfo* -> 0 / -1 */
    SYS_MOUNTS   = 7,  /* a0 = task id, a1 = buf, a2 = cap -> bytes */
    SYS_MEMINFO  = 8,  /* a0 = int[2] out: {free pages, total pages} */
    /* Namespace mutation: also kernel-side per-task state. Plan 9's two
       operations, kept apart because they are not the same thing: mount puts
       a server behind a name, bind makes one name mean another. */
    SYS_BIND     = 9,  /* a0 = old path, a1 = new path, a2 = flags */
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
    /* One zeroed page, mapped into the caller and reported with both its
       virtual and its physical address. Devices are programmed with the
       latter; nothing else in the system is allowed to care. */
    SYS_DMA_ALLOC = 17,   /* a0 = struct dmapage* -> 0 / -1 */
    /* Wake me later. The only way a task can act on the passage of time:
       without it a driver that blocks waiting for a reply waits forever, and
       nothing built on top can ever decide that something was lost. */
    SYS_ALARM   = 18,     /* a0 = milliseconds, 0 cancels */
    /* Is that task still that task? A server holding state on behalf of a
       client — an open file, a request it has not answered — has no other way
       to find out that the client is gone. Ids carry a generation, so this
       answers "no" for a slot that has been reused as well as for one that is
       empty. */
    SYS_ALIVE   = 19,     /* a0 = task id -> 1 or 0 */
    SYS_MOUNT   = 20,     /* a0 = prefix, a1 = server, a2 = flags */
    SYS_UNMOUNT = 21,     /* a0 = name — take it back */
    /* send, but -1 rather than blocking if the destination is not in a
       recv. What a server uses to answer a request it parked, since it
       must never be left waiting on a client. */
    SYS_TRYSEND = 22,     /* a0 = dest, a1 = message, a2 = length */
    /* Block until that task is gone. Polling sys_alive would do, and did for
       a while, but it burns a slice per check and a shell running a command
       spends all its time there. */
    SYS_WAIT    = 23,     /* a0 = task id -> 0 when it has exited */
    /* recv, but from one named task and nobody else. A reply is not an event:
       whoever sent a request knows who owes the answer, and taking the next
       message from anybody is not a race but a wrong answer. Anything from
       another sender stays queued. */
    SYS_RECVFROM = 24,   /* a0 = sender, a1 = buffer, a2 = cap */

    /* Move this task's break by a0 bytes and answer with where it was. The
       kernel's entire contribution to having an allocator: pages, mapped and
       unmapped, at a fixed window in the address space. */
    SYS_SBRK    = 25,

    /* Stop that task. It is exactly what a fault does to one — the same
       task_retire, which wakes whoever was waiting on it, tells a closed
       receiver its correspondent is gone, and gives the pages back. Nothing
       is delivered to the task itself: there are no signals here, and a task
       that could refuse to die is a larger idea than this needs.

       Unguarded, like SYS_NEWTASK and for the same reason: this system has no
       notion of who owns whom. That is a real hole and it is written down
       rather than hidden — see the README. */
    SYS_KILL    = 26,     /* a0 = task id -> 0, or -1 if there is no such task */

    /* What the machine said about itself. The device tree and the PCI
       configuration space are physical addresses the kernel maps and a user
       task does not — so, like the task table and the page tables, the facts
       are handed over as text and the formatting is left to whoever asked.
       a0 picks the question, a1 the index where one applies. */
    SYS_DEVINFO = 27,     /* a0 = what, a1 = index, a2 = buf, a3 = cap */
};

enum {
    DEVINFO_TREE = 0,     /* the flattened device tree, rendered */
    DEVINFO_NAME,         /* "00:01.0" for pci function a1, -1 past the end */
    DEVINFO_PCI,          /* that function's vendor, class, bars, irq */
};

/* sys_recv() returns this instead of a task id when what arrived was an
   interrupt rather than a message. A driver's receive loop then handles both
   without needing a second blocking primitive. */
#define IRQ_SENDER   (-2)
/* ...and this when the alarm went off. Three kinds of event, one blocking
   call: a driver needs no separate mechanism for any of them. */
#define TIMER_SENDER (-3)

/* Both halves of a DMA mapping. One page: the split virtqueues we drive keep
   each ring area and each packet buffer inside a single page, so nothing here
   needs physically contiguous runs — which is fortunate, because the page
   allocator is a free list and cannot promise them. */
struct dmapage {
    uint64 va;
    uint64 pa;
};

/* Why a task is not running, which "blocked" does not say.

   Every rendezvous in this system is a pair of tasks, and until now the pair
   was invisible: `ps` said blocked and left you to guess whether a task was
   waiting for a reply, waiting for a client, or wedged against another task
   that was waiting for it. The kernel has known all along — these are the
   fields ipc.c parks a task with — and it costs four integers to say. */
enum {
    IPC_NONE = 0,        /* running, or blocked on something that is not IPC */
    IPC_RECV,            /* in recv(): anybody may answer */
    IPC_RECVFROM,        /* in recv_from(peer): only that one may */
    IPC_SEND,            /* in send(peer): parked on its queue */
    IPC_WAIT,            /* in wait(peer): until that task is gone */
    IPC_ALARM,           /* in an alarm: until the clock says so */
};

struct taskinfo {
    int  id;
    int  state;          /* matches enum task_state */
    int  is_current;
    char name[16];
    int  ipc;            /* one of the above */
    int  peer;           /* whom, where that means anything */
    int  msglen;         /* the message it is holding out, for IPC_SEND */
    int  senders;        /* how many tasks are queued sending to it */
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

static inline void *sys_sbrk(long delta)
{
    return (void *)_ecall1(SYS_SBRK, delta);
}

static inline int sys_taskinfo(int idx, struct taskinfo *out)
{
    return (int)_ecall2(SYS_TASKINFO, idx, (long)out);
}
/* task_id < 0 asks about the caller's own namespace, which is the only one a
   program can be sure of the number of. */
static inline int sys_mounts(int task_id, char *out, int cap)
{
    return (int)_ecall3(SYS_MOUNTS, task_id, (long)out, cap);
}
static inline int sys_meminfo(int out[2])
{
    return (int)_ecall1(SYS_MEMINFO, (long)out);
}
static inline int sys_mount(const char *prefix, int server_task, int flags)
{
    return (int)_ecall3(SYS_MOUNT, (long)prefix, server_task, flags);
}
static inline int sys_unmount(const char *name)
{
    return (int)_ecall1(SYS_UNMOUNT, (long)name);
}
/* bind(old, new): `new` means `old` from now on. The second argument is the
   one that changes — Plan 9's order, and the one everybody gets backwards. */
static inline int sys_bind(const char *old, const char *new, int flags)
{
    return (int)_ecall3(SYS_BIND, (long)old, (long)new, flags);
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

static inline int sys_alarm(int ms)
{
    return (int)_ecall1(SYS_ALARM, ms);
}

static inline int sys_dma_alloc(struct dmapage *out)
{
    return (int)_ecall1(SYS_DMA_ALLOC, (long)out);
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

static inline int sys_resolve(const char *path, char *out, int cap, int nth)
{
    return (int)_ecall4(SYS_RESOLVE, (long)path, (long)out, cap, nth);
}

/* Render one task's translation of `va` as text into our own buffer. */
static inline int sys_pgdump(int task_id, unsigned long va, void *out, int cap)
{
    return (int)_ecall4(SYS_PGDUMP, task_id, (long)va, (long)out, cap);
}

/* Blocks until the destination receives; the kernel copies `len` bytes out
   of this task's address space into the receiver's. Returns -1 if there is no
   such task — which is not a formality: a caller that ignored it would go on
   to block in recv() waiting for a reply from nobody. */
static inline int sys_send(int dst, const void *msg, int len)
{
    return (int)_ecall3(SYS_SEND, dst, (long)msg, len);
}

static inline int sys_devinfo(int what, int index, char *out, int cap)
{
    return (int)_ecall4(SYS_DEVINFO, what, index, (long)out, cap);
}

static inline int sys_kill(int task_id)
{
    return (int)_ecall1(SYS_KILL, task_id);
}

static inline int sys_wait(int task_id)
{
    return (int)_ecall1(SYS_WAIT, task_id);
}

static inline int sys_trysend(int dst, const void *msg, int len)
{
    return (int)_ecall3(SYS_TRYSEND, dst, (long)msg, len);
}

/* Whether a task id still names the task it named when it was handed out. */
static inline int sys_alive(int task_id)
{
    return (int)_ecall1(SYS_ALIVE, task_id);
}

/* Blocks until a message arrives from anybody; returns the sender's task id,
   or IRQ_SENDER / TIMER_SENDER. What a server and a driver wait on. */
static inline int sys_recv(void *buf, int len)
{
    return (int)_ecall2(SYS_RECV, (long)buf, len);
}

/* Blocks until *that* task sends. -1 if it is already gone, or dies while
   this is waiting — otherwise a client whose server died would wait for ever
   with somebody else's message queued behind it. */
static inline int sys_recv_from(int src, void *buf, int len)
{
    return (int)_ecall3(SYS_RECVFROM, src, (long)buf, len);
}
