/* task.c — fixed table of tasks with a preemptive round-robin scheduler.
   Each task owns an Sv39 address space: the kernel image and MMIO are shared
   (kernel code has to run when a trap lands), but the stack is private and
   the free-page arena is not mapped at all, so one task holds no translation
   for another's memory. Isolation is why IPC copies rather than sharing. */
#include "task.h"
#include "uart.h"
#include "syscall.h"
#include "vfs.h"
#include "vm.h"
#include "pmm.h"

struct task tasks[NTASK];
struct task *current;
static int ntasks;

extern void _ret_to_task(void) __attribute__((noreturn));

struct task *task_create(const char *name, void (*entry)(void))
{
    struct task *t = &tasks[ntasks];
    for (int i = 0; i < 32; i++)
        t->ctx.x[i] = 0;

    /* Its own address space, and a stack that exists only inside it. Every
       task's stack is at USTACK_TOP, yet on different physical pages, so the
       identical address means something different depending on who asks. */
    t->pt = vm_create_task_pt();
    for (int i = 0; i < USTACK_PAGES; i++) {
        void *p = pmm_alloc();
        vm_map_at(t->pt, USTACK_TOP - (uint64)(i + 1) * PGSIZE,
                  (uint64)p, PGSIZE, PTE_R | PTE_W, 0);
    }

    t->ctx.x[2]   = USTACK_TOP;                        /* sp -> stack top */
    t->ctx.satp   = MAKE_SATP(t->pt);
    t->ctx.epc    = (uint64)entry;
    /* sret returns to S-mode (SPP=1) with interrupts enabled (SPIE -> SIE) */
    t->ctx.status = SSTATUS_SPP | SSTATUS_SPIE;
    t->state = T_RUNNABLE;
    t->id    = ntasks;
    t->name  = name;
    t->ns    = vfs_root_ns();   /* inherit the shared view until it clones */
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
    /* The loop above also reconsiders `current` itself (i == NTASK), so a task
       that just blocked or was retired is not picked. With an always-runnable
       idle task this point is unreachable; if it is ever reached, park rather
       than resurrecting a dead task — doing that turned a page fault into an
       endless refault loop. */
    for (;;)
        __asm__ volatile("wfi");
}

void yield(void)
{
    sys_yield();        /* ecall -> _mtrap saves context -> schedule() */
}

/* Dispatched from trap.c on an S-mode ecall. sepc has already been advanced
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
    case SYS_PGDUMP: {
        /* Only the kernel can do this: it runs with the kernel table
           installed, which is the one address space that still reaches the
           page-table pages themselves. */
        static char kbuf[512];
        int    tid = (int)current->ctx.x[10];
        uint64 va  = current->ctx.x[11];
        uint64 out = current->ctx.x[12];
        int    cap = (int)current->ctx.x[13];
        pagetable_t pt = (tid >= 0 && tid < NTASK && tasks[tid].pt)
                         ? tasks[tid].pt : kernel_pagetable;
        int n = vm_dump_walk_in(pt, va, kbuf, (int)sizeof(kbuf));
        if (n > cap) n = cap;
        if (n > 0)
            vm_copy_across(current->pt, out,
                           kernel_pagetable, (uint64)kbuf, (uint64)n);
        current->ctx.x[10] = (uint64)n;
        break;
    }
    default:
        if (!ipc_syscall(num))
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
