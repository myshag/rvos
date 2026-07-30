/* plic.c — Platform-Level Interrupt Controller for the QEMU 'virt' board.

   The register map is per *context*, and a context is a (hart, privilege)
   pair: hart 0 machine mode is context 0, hart 0 supervisor mode is context
   1, and so on. rvos lives in supervisor mode on hart 0, so every offset
   below is the S-mode one — using the M-mode context by mistake produces a
   controller that looks configured and never delivers anything. */
#include "plic.h"

#define PLIC_BASE           0x0c000000UL

#define PLIC_PRIORITY(irq)  (PLIC_BASE + 4UL * (irq))
#define PLIC_SENABLE(hart)  (PLIC_BASE + 0x2080UL + (hart) * 0x100UL)
#define PLIC_STHRESH(hart)  (PLIC_BASE + 0x201000UL + (hart) * 0x2000UL)
#define PLIC_SCLAIM(hart)   (PLIC_BASE + 0x201004UL + (hart) * 0x2000UL)

/* mhartid is a machine-mode CSR and cannot be read from here. boot.S parks
   every hart but the first, so the context we want is always hart 0's. */
#define HART 0

static inline void w32(uint64 a, uint32 v) { *(volatile uint32 *)a = v; }
static inline uint32 r32(uint64 a)         { return *(volatile uint32 *)a; }

void plic_init(void)
{
    /* Priority 0 means "never deliver", so a source stays silent until it is
       given one. Everything we care about gets the same weight. */
    w32(PLIC_PRIORITY(UART0_IRQ), 1);
    for (int i = VIRTIO_IRQ_BASE; i < VIRTIO_IRQ_BASE + 8; i++)
        w32(PLIC_PRIORITY(i), 1);
}

void plic_init_hart(void)
{
    uint32 en = (1u << UART0_IRQ);
    for (int i = VIRTIO_IRQ_BASE; i < VIRTIO_IRQ_BASE + 8; i++)
        en |= (1u << i);
    w32(PLIC_SENABLE(HART), en);

    w32(PLIC_STHRESH(HART), 0);       /* accept anything above priority 0 */
    w_sie(r_sie() | SIE_SEIE);     /* and let S-mode see external interrupts */
}

int plic_claim(void)
{
    return (int)r32(PLIC_SCLAIM(HART));
}

void plic_complete(int irq)
{
    w32(PLIC_SCLAIM(HART), (uint32)irq);
}
