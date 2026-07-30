/* trap.c — supervisor-mode trap handling: timer, syscalls, page faults.

   The timer comes from Sstc (the stimecmp CSR) rather than the CLINT. That
   is not a stylistic choice: CLINT's mtimecmp is machine-mode only and the
   machine timer interrupt cannot be delegated, so an S-mode kernel either
   uses Sstc or has M-mode forward every tick by hand. mstart() enables it
   via menvcfg.STCE. */
#include "riscv.h"
#include "uart.h"
#include "task.h"
#include "plic.h"
#include "syscall.h"

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

/* Every timer tick, see whose alarm has come due. Waking a task looks exactly
   like delivering a message to it, which is what lets one blocking call serve
   messages, interrupts and timeouts alike. */
static void alarms_check(void)
{
    uint64 now = r_time();
    for (int i = 0; i < NTASK; i++) {
        struct task *t = &tasks[i];
        if (t->state == T_UNUSED || !t->alarm_at || now < t->alarm_at)
            continue;
        t->alarm_at = 0;
        if (t->state == T_BLOCKED && t->waiting_recv) {
            t->ctx.x[10]    = (uint64)(long)TIMER_SENDER;
            t->waiting_recv = 0;
            t->state        = T_RUNNABLE;
        } else {
            t->timer_pending = 1;
        }
    }
}

/* Which task drives which interrupt. Nothing here knows what the devices
   are — only who asked for them. */
#define NIRQ 32
static struct task *irq_owner[NIRQ];

int irq_register(int irq, struct task *t)
{
    if (irq <= 0 || irq >= NIRQ)
        return -1;
    irq_owner[irq] = t;
    return 0;
}

int irq_ack(int irq)
{
    if (irq <= 0 || irq >= NIRQ || irq_owner[irq] != current)
        return -1;
    plic_complete(irq);            /* unmask: it may fire again */
    return 0;
}

/* A device fired. Claim it, hand it to whoever registered for it, and leave
   it masked — the PLIC will not raise it again until the driver acks, which
   is exactly the back-pressure that stops an interrupt storm while a
   user-mode driver is being scheduled.

   The kernel deliberately does not touch the device. It does not know how. */
static void device_interrupt(void)
{
    int irq = plic_claim();
    if (irq == 0)
        return;                    /* spurious */

    struct task *t = (irq > 0 && irq < NIRQ) ? irq_owner[irq] : 0;
    if (!t || t->state == T_UNUSED) {
        /* Nobody owns it. Completing keeps the line quiet; there is nothing
           sensible to do with the data. */
        plic_complete(irq);
        return;
    }

    if (t->state == T_BLOCKED && t->waiting_recv) {
        t->ctx.x[10]     = (uint64)(long)IRQ_SENDER;   /* recv() -> "an irq" */
        t->waiting_recv  = 0;
        t->state         = T_RUNNABLE;
    } else {
        t->irq_pending = 1;        /* busy; it will see this on its next recv */
    }
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
            alarms_check();
            schedule();                 /* preempt: may switch `current` */
        } else if ((scause & 0xff) == IRQ_S_EXTERNAL) {
            device_interrupt();
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
