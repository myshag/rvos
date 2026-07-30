/* pmm.c — physical page allocator. Free pages are kept on a linked list
   threaded through the pages themselves: a free page's first word is the
   pointer to the next free page, so the allocator needs no separate metadata.
   That trick only works while RAM is identity-mapped (VA == PA), which is
   exactly how the kernel maps itself. */
#include "pmm.h"
#include "util.h"

struct run { struct run *next; };

static struct run *freelist;
static int nfree, ntotal;

extern char _end[];            /* first byte past the kernel image */

void pmm_free(void *page)
{
    uint64 p = (uint64)page;
    if (p % PGSIZE || p < RAM_BASE || p >= PMM_TOP)
        return;                /* refuse anything not a page in our arena */
    struct run *r = (struct run *)page;   /* zeroing happens in alloc */
    r->next = freelist;
    freelist = r;
    nfree++;
}

void pmm_init(void)
{
    uint64 start = PGROUNDUP((uint64)_end);
    for (uint64 p = start; p + PGSIZE <= PMM_TOP; p += PGSIZE) {
        pmm_free((void *)p);
        ntotal++;
    }
}

void *pmm_alloc(void)
{
    struct run *r = freelist;
    if (!r)
        return 0;
    freelist = r->next;
    nfree--;
    memset(r, 0, PGSIZE);      /* callers (page tables) require zeroed pages */
    return r;
}

int pmm_free_count(void)  { return nfree; }
int pmm_total_count(void) { return ntotal; }
