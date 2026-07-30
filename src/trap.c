/* trap.c — C side of machine-mode trapping: timer setup + dispatch. */
#include "riscv.h"
#include "uart.h"
#include "task.h"

/* Ticks between preemptions. QEMU CLINT runs at 10 MHz, so ~0.05 s. */
#define TICK 500000UL

extern void _mtrap(void);

static void timer_rearm(void)
{
    uint64 h = r_mhartid();
    mmio_w64(CLINT_MTIMECMP(h), mmio_r64(CLINT_MTIME) + TICK);
}

void trap_init(void)
{
    w_mtvec((uint64)_mtrap);
}

void timer_init(void)
{
    timer_rearm();
    w_mie(r_mie() | MIE_MTIE);          /* enable machine timer interrupt */
}

/* Called from trap.S with all state already saved into current->ctx. */
void mtrap_handler(void)
{
    uint64 mcause = r_mcause();

    if (mcause & MCAUSE_INT) {
        if ((mcause & 0xff) == IRQ_M_TIMER) {
            timer_rearm();
            schedule();                 /* preempt: may switch `current` */
        }
        return;
    }

    if (mcause == 11) {                 /* environment call from M-mode */
        current->ctx.mepc += 4;         /* resume past the ecall */
        syscall_dispatch(current->ctx.x[17]);   /* a7 = syscall number */
        return;
    }

    /* Synchronous exception — fatal in this teaching kernel. */
    kprintf("\n[panic] exception mcause=%lx mepc=%lx mtval=%lx task=%s\n",
            mcause, r_mepc(), r_mtval(), current ? current->name : "?");
    for (;;)
        __asm__ volatile("wfi");
}
