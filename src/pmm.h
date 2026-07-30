#pragma once
#include "riscv.h"

/* Physical page allocator: hands out zeroed 4 KiB pages, which is the only
   granularity Sv39 page tables (and the tables themselves) are made of. */
void   pmm_init(void);
void  *pmm_alloc(void);        /* zeroed page, or 0 when exhausted */
void   pmm_free(void *page);
int    pmm_free_count(void);
int    pmm_total_count(void);
