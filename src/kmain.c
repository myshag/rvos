/* kmain.c — Stage 2: bring up traps + timer, create two tasks, and hand off to
   the preemptive scheduler. Interleaved A/B output proves timer preemption. */
#include "uart.h"
#include "task.h"
#include "syscall.h"

static void spin(volatile unsigned long n)
{
    while (n--)
        __asm__ volatile("nop");
}

static void task_a(void)
{
    for (unsigned long i = 0;; i++) {
        kprintf("  [A] step %lu\n", i);
        spin(2000000);
        if ((i & 3) == 3)
            yield();            /* also exercise the voluntary path */
    }
}

static void task_b(void)
{
    for (unsigned long i = 0;; i++) {
        kprintf("  [B] step %lu\n", i);
        spin(3000000);
    }
}

void kmain(void)
{
    uart_init();
    kprintf("\n=============================================\n");
    kprintf("  rvos — educational RISC-V microkernel\n");
    kprintf("  stage 2: traps + timer + preemptive RR\n");
    kprintf("=============================================\n");

    trap_init();
    timer_init();

    task_create("A", task_a);
    task_create("B", task_b);
    kprintf("[boot] 2 tasks created; starting scheduler.\n");

    scheduler_start();          /* never returns */
}
