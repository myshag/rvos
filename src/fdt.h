#pragma once
#include "riscv.h"

/* The flattened device tree QEMU hands the kernel in a1. */

extern unsigned long dtb_pa;    /* boot.S puts it here, before anything else */

int  fdt_init(void);            /* 0 if there is a tree and it looks like one */

/* The first node whose name starts with `prefix` ("serial@", "pci@"), and its
   `reg` — a base and a size. -1 if there is no such node. */
int  fdt_reg(const char *prefix, uint64 *base, uint64 *size);

/* Same, but the nth such node, and its interrupt number if it has one. */
int  fdt_reg_n(const char *prefix, int n, uint64 *base, uint64 *size, int *irq);

/* How many nodes there are whose name starts with `prefix`. */
int  fdt_count(const char *prefix);

/* The whole tree as text, in the listing shape the rest of the system uses
   for anything a person is going to read. */
int  fdt_render(char *out, int cap);
