#pragma once
#include "riscv.h"

/* Full integer context saved on every trap. `x` is indexed by register number
   (x[0] unused). Offsets are mirrored in entry.S — keep in sync:
   x[i] at i*8, epc at 256, status at 264, satp at 272. */
struct context {
    uint64 x[32];
    uint64 epc;         /* sepc:    where the task resumes */
    uint64 status;      /* sstatus: privilege + interrupt state on sret */
    uint64 satp;        /* the task's address space, installed on the way out */
};

enum task_state { T_UNUSED = 0, T_RUNNABLE, T_RUNNING, T_BLOCKED };

struct namespace;   /* opaque here; defined in vfs.c */

#define NTASK   8

/* Every task's stack sits at the same virtual address but on different
   physical pages — which is the point: task A simply has no mapping for
   task B's stack, so it cannot reach it however hard it tries. */
#define USTACK_TOP   0x30000000UL
#define USTACK_PAGES 4                       /* 16 KiB */

struct task {
    struct context ctx;                      /* MUST be first (offset 0) */
    pagetable_t pt;                          /* this task's address space */
    enum task_state state;
    int    id;
    const char *name;
    struct namespace *ns;       /* what paths mean to this task (Plan 9) */
    /* IPC: rendezvous message passing. Now that address spaces differ, a
       message is an (address, length) pair in the *owner's* address space
       and the kernel copies it across; a bare pointer would be meaningless
       on the other side. */
    uint64 send_va;             /* blocked sender: where its message lives */
    int    send_len;
    uint64 recv_va;             /* blocked receiver: where it wants one */
    int    recv_len;
    int    waiting_recv;        /* 1 => blocked in recv (vs. blocked in send) */
    struct task *wait_sender;   /* head of senders blocked on us as receiver */
    struct task *send_next;     /* intrusive link within that sender queue */
};

extern struct task *current;
extern struct task tasks[NTASK];   /* the table itself, so /proc can read it */

struct task *task_create(const char *name, void (*entry)(void));
void schedule(void);
void scheduler_start(void) __attribute__((noreturn));
void yield(void);                    /* voluntary reschedule */
void syscall_dispatch(uint64 num);   /* called from trap.c on ecall */

/* trap / timer bring-up (trap.c) */
void trap_init(void);
void timer_init(void);

/* IPC (ipc.c) — returns 1 if it handled `num`. */
int ipc_syscall(uint64 num);
