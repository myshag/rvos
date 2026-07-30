#pragma once
#include "riscv.h"

extern pagetable_t kernel_pagetable;

void   vm_init(void);       /* build the kernel table and switch satp to it */

/* Map `size` bytes at the given leaf level: 0 = 4 KiB pages, 1 = 2 MiB
   superpages, 2 = 1 GiB. va/pa must be aligned to that level's page size. */
int    vm_map_at(pagetable_t pt, uint64 va, uint64 pa, uint64 size,
                 uint64 perm, int level);

uint64 vm_translate(uint64 va);                  /* PA, or 0 if unmapped */
int    vm_dump_walk(uint64 va, char *out, int cap);  /* the walk, as text */
