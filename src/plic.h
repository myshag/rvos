#pragma once
#include "riscv.h"

/* PLIC — the Platform-Level Interrupt Controller. Every device interrupt on
   the 'virt' board arrives through it, so nothing outside the timer can reach
   the kernel until this is set up. It is the missing piece between "a device
   raised a line" and "a task got told". */

void plic_init(void);        /* global: give each source a priority */
void plic_init_hart(void);   /* per-hart: enable sources, drop the threshold */

/* claim returns the highest-priority pending interrupt (0 if none) and marks
   it in-service; complete tells the PLIC it may fire again. Between the two
   the source is effectively masked, which is what lets the kernel hand the
   interrupt to a user-mode driver and wait for it to answer. */
int  plic_claim(void);
void plic_complete(int irq);
