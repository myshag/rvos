/* trap.c — supervisor-mode trap handling: timer, syscalls, page faults.

   The timer comes from Sstc (the stimecmp CSR) rather than the CLINT. That
   is not a stylistic choice: CLINT's mtimecmp is machine-mode only and the
   machine timer interrupt cannot be delegated, so an S-mode kernel either
   uses Sstc or has M-mode forward every tick by hand. mstart() enables it
   via menvcfg.STCE. */
#include "riscv.h"
#include "uart.h"
#include "task.h"

/* QEMU's time base is 10 MHz, so this preempts roughly every 50 ms. */
#define TICK 500000UL

extern void _strap(void);

static void timer_rearm(void)
{
    w_stimecmp(r_time() + TICK);
}

void trap_init(void)
{
    w_stvec((uint64)_strap);
}

void timer_init(void)
{
    timer_rearm();
    w_sie(r_sie() | SIE_STIE);
}

static const char *cause_name(uint64 c)
{
    switch (c) {
    case EXC_INST_PAGE_FAULT:  return "instruction page fault";
    case EXC_LOAD_PAGE_FAULT:  return "load page fault";
    case EXC_STORE_PAGE_FAULT: return "store page fault";
    case 2:  return "illegal instruction";
    case 5:  return "load access fault";
    case 7:  return "store access fault";
    default: return "exception";
    }
}

/* Called from entry.S with the full context already saved into current->ctx. */
void strap_handler(void)
{
    uint64 scause = r_scause();

    if (scause & CAUSE_INT) {
        if ((scause & 0xff) == IRQ_S_TIMER) {
            timer_rearm();
            schedule();                 /* preempt: may switch `current` */
        }
        return;
    }

    if (scause == EXC_ECALL_S || scause == EXC_ECALL_U) {
        current->ctx.epc += 4;          /* resume past the ecall */
        syscall_dispatch(current->ctx.x[17]);   /* a7 = syscall number */
        return;
    }

    /* A fault the MMU raised. Rather than panicking the whole kernel we
       report it and retire the offending task — which is what lets the demo
       deliberately write to a read-only page and live to describe it. */
    kprintf("\n[trap] %s in task '%s'\n", cause_name(scause),
            current ? current->name : "?");
    kprintf("       scause=%ld  stval=0x%lx  sepc=0x%lx\n",
            scause, r_stval(), current ? current->ctx.epc : 0);
    kprintf("       -> the MMU refused it; retiring the task\n\n");

    if (current)
        task_retire(current);       /* its pages go back to the allocator */
    schedule();
}
