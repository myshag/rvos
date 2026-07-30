#pragma once
#include "riscv.h"

/* Full integer context saved on every trap. `x` is indexed by register number
   (x[0] unused); mepc/mstatus follow. Offsets are mirrored in trap.S — keep in
   sync: x[i] at i*8, mepc at 256, mstatus at 264. */
struct context {
    uint64 x[32];
    uint64 mepc;
    uint64 mstatus;
};

enum task_state { T_UNUSED = 0, T_RUNNABLE, T_RUNNING, T_BLOCKED };

#define NTASK   8
#define TSTACK  8192

struct task {
    struct context ctx;                          /* MUST be first (offset 0) */
    uint8  stack[TSTACK] __attribute__((aligned(16)));
    enum task_state state;
    int    id;
    const char *name;
    /* IPC (stage 3) */
    struct task *ipc_from;      /* who we are blocked receiving/sending to */
    uint64 ipc_msg;             /* transferred message word */
    struct task *wait_sender;   /* queue head of tasks blocked sending to us */
    struct task *send_next;     /* intrusive link within a receiver's queue */
};

extern struct task *current;

struct task *task_create(const char *name, void (*entry)(void));
void schedule(void);
void scheduler_start(void) __attribute__((noreturn));
void yield(void);                    /* voluntary reschedule */
void syscall_dispatch(uint64 num);   /* called from trap.c on ecall */

/* trap / timer bring-up (trap.c) */
void trap_init(void);
void timer_init(void);
