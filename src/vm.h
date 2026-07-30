#pragma once
#include "riscv.h"

extern pagetable_t kernel_pagetable;
extern uint64      kernel_satp;   /* entry.S installs this on every trap */

void   vm_init(void);       /* build the kernel table and switch satp to it */

/* Map `size` bytes at the given leaf level: 0 = 4 KiB pages, 1 = 2 MiB
   superpages, 2 = 1 GiB. va/pa must be aligned to that level's page size. */
int    vm_map_at(pagetable_t pt, uint64 va, uint64 pa, uint64 size,
                 uint64 perm, int level);

/* A fresh address space for a task: the kernel image and MMIO, and nothing
   else — no other task's pages, and none of the free-page arena. */
pagetable_t vm_create_task_pt(void);

uint64 vm_translate(uint64 va);                       /* in the kernel table */
uint64 vm_translate_in(pagetable_t pt, uint64 va);    /* in any table */

/* Copy between two address spaces. Only safe while the kernel table is
   installed, since it is what makes every physical page reachable. */
int    vm_copy_across(pagetable_t dpt, uint64 dva,
                      pagetable_t spt, uint64 sva, uint64 len);

int    vm_dump_walk(uint64 va, char *out, int cap);
int    vm_dump_walk_in(pagetable_t pt, uint64 va, char *out, int cap);
