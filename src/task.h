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

#define NTASK   18

/* USTACK_TOP / USTACK_PAGES live in riscv.h: every task's stack sits at the
   same virtual address but on different physical pages — which is the point:
   task A simply has no mapping for task B's stack. */

struct task {
    struct context ctx;                      /* MUST be first (offset 0) */
    pagetable_t pt;                          /* this task's address space */
    enum task_state state;
    /* An id is a slot index and a generation together. The slot alone is not
       enough: slots are reused, and anything that remembered "task 6" — a
       server holding a request it has not answered, a mount table entry —
       would go on believing it after task 6 died and something else moved in.
       The generation makes a stale id name nobody rather than the wrong
       somebody, which is the difference between a request that fails and a
       reply delivered to an innocent task. */
    int    id;
    int    gen;                 /* bumped each time this slot is handed out */
    const char *name;
    char   namebuf[16];         /* for names that arrive by syscall */
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
    int    irq_pending;         /* a device it drives fired while it was busy */
    uint64 dma_next;            /* next free VA in its DMA window */
    uint64 alarm_at;            /* absolute time to wake it, 0 = no alarm */
    int    timer_pending;       /* the alarm went off while it was busy */
    struct task *wait_sender;   /* head of senders blocked on us as receiver */
    struct task *send_next;     /* intrusive link within that sender queue */
};

extern struct task *current;
extern struct task tasks[NTASK];   /* the table itself, so /proc can read it */

/* The only correct way to turn an id back into a task: it checks the slot is
   in use and that the generation still matches. Indexing tasks[] directly
   with an id is the bug this exists to prevent. */
#define TASK_SLOT(id)  ((id) & 0xff)
#define TASK_GEN(id)   ((id) >> 8)
struct task *task_by_id(int id);   /* 0 if that task is gone */

struct task *task_create(const char *name, void (*entry)(void));
struct task *task_create_user(const char *name, void (*entry)(void),
                              void *data_start, void *data_end);
struct task *task_new_empty(const char *name);   /* for the ELF loader */
void task_retire(struct task *t);   /* free its memory, release the slot */
void schedule(void);
void scheduler_start(void) __attribute__((noreturn));
void yield(void);                    /* voluntary reschedule */
void syscall_dispatch(uint64 num);   /* called from trap.c on ecall */

/* interrupt ownership (trap.c) */
int irq_register(int irq, struct task *t);
int irq_ack(int irq);

/* trap / timer bring-up (trap.c) */
void trap_init(void);
void timer_init(void);

/* IPC (ipc.c) — returns 1 if it handled `num`. */
int ipc_syscall(uint64 num);
