/* task.c — fixed table of tasks with a preemptive round-robin scheduler.
   No MMU/paging: tasks share one address space and cooperate via IPC (stage 3).
   That makes rvos "microkernel-style" (message passing) rather than isolated;
   Sv39 per-task page tables are the natural next step. */
#include "task.h"
#include "uart.h"
#include "syscall.h"

struct task tasks[NTASK];
struct task *current;
static int ntasks;

extern void _ret_to_task(void) __attribute__((noreturn));

struct task *task_create(const char *name, void (*entry)(void))
{
    struct task *t = &tasks[ntasks];
    for (int i = 0; i < 32; i++)
        t->ctx.x[i] = 0;
    t->ctx.x[2]   = (uint64)(t->stack + TSTACK);       /* sp -> stack top */
    t->ctx.mepc   = (uint64)entry;
    t->ctx.mstatus = MSTATUS_MPP_M | MSTATUS_MPIE;     /* mret -> M-mode, MIE=1 */
    t->state = T_RUNNABLE;
    t->id    = ntasks;
    t->name  = name;
    ntasks++;
    return t;
}

/* Round-robin: from the task after `current`, pick the next RUNNABLE one. */
void schedule(void)
{
    int start = current ? current->id : -1;

    if (current && current->state == T_RUNNING)
        current->state = T_RUNNABLE;

    for (int i = 1; i <= NTASK; i++) {
        struct task *t = &tasks[(start + i) % NTASK];
        if (t->state == T_RUNNABLE) {
            t->state = T_RUNNING;
            current = t;
            return;
        }
    }
    /* Nothing runnable: keep the old current alive (it may be RUNNING still).
       If everyone is blocked, resume current so we return from the trap. */
    if (current)
        current->state = T_RUNNING;
}

void yield(void)
{
    sys_yield();        /* ecall -> _mtrap saves context -> schedule() */
}

/* Dispatched from trap.c on an M-mode ecall. mepc has already been advanced
   past the ecall; args live in the saved context (a0=x[10], a1=x[11], ...). */
void syscall_dispatch(uint64 num)
{
    switch (num) {
    case SYS_YIELD:
        schedule();
        break;
    case SYS_PUTC:
        uart_putc((char)current->ctx.x[10]);
        break;
    default:
        kprintf("[warn] task %s: bad syscall %ld\n", current->name, num);
        break;
    }
}

void scheduler_start(void)
{
    current = 0;
    schedule();
    _ret_to_task();
}
