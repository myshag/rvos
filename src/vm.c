/* vm.c — Sv39 virtual memory.

   A virtual address is 39 bits, sign-extended to 64:

       [63:39] copy of bit 38 | VPN[2] 9b | VPN[1] 9b | VPN[0] 9b | offset 12b

   Translation walks a three-level tree. satp holds the physical page number
   of the root table; each table is one 4 KiB page of 512 eight-byte PTEs
   (512 = 2^9, which is why each index is 9 bits). A PTE with R=W=X=0 is a
   pointer to the next table; any of R/W/X set makes it a leaf and the walk
   stops there — so a leaf at level 1 covers 2 MiB and at level 2 covers
   1 GiB, with the remaining VPN bits folding into the offset. Superpages
   are not a separate mechanism, just an early stop.

   Note this file could not do anything at all in M-mode: satp is ignored
   there and every address is physical. Paging is why rvos drops to S-mode. */
#include "vm.h"
#include "pmm.h"
#include "util.h"
#include "uart.h"
#include "elf.h"
#include "task.h"

pagetable_t kernel_pagetable;
uint64      kernel_satp;

extern char _end[];

/* Descend to the PTE for `va` at `stop_level`, optionally allocating the
   intermediate tables. Returns 0 if a superpage already covers the path. */
static pte_t *walk(pagetable_t pt, uint64 va, int alloc, int stop_level)
{
    for (int level = 2; level > stop_level; level--) {
        pte_t *pte = &pt[PX(level, va)];
        if (*pte & PTE_V) {
            if (PTE_IS_LEAF(*pte))
                return 0;                    /* a superpage blocks the way */
            pt = (pagetable_t)PTE2PA(*pte);
        } else {
            if (!alloc)
                return 0;
            pagetable_t next = pmm_alloc();
            if (!next)
                return 0;
            /* pointer PTE: valid, but no R/W/X — that is what makes it a
               link rather than a leaf */
            *pte = PA2PTE(next) | PTE_V;
            pt = next;
        }
    }
    return &pt[PX(stop_level, va)];
}

int vm_map_at(pagetable_t pt, uint64 va, uint64 pa, uint64 size,
              uint64 perm, int level)
{
    uint64 psz = PGSIZE << (9 * level);
    uint64 a    = va & ~(psz - 1);
    uint64 last = (va + size - 1) & ~(psz - 1);

    for (;;) {
        pte_t *pte = walk(pt, a, 1, level);
        if (!pte)
            return -1;
        /* A and D are set up front so we never take an accessed/dirty fault;
           they grant nothing — permission still comes from R/W/X. */
        *pte = PA2PTE(pa) | perm | PTE_V | PTE_A | PTE_D;
        if (a == last)
            break;
        a  += psz;
        pa += psz;
    }
    return 0;
}

uint64 vm_translate_in(pagetable_t pt, uint64 va)
{
    for (int level = 2; level >= 0; level--) {
        pte_t *pte = &pt[PX(level, va)];
        if (!(*pte & PTE_V))
            return 0;
        if (PTE_IS_LEAF(*pte)) {
            uint64 psz = PGSIZE << (9 * level);
            return PTE2PA(*pte) | (va & (psz - 1));
        }
        pt = (pagetable_t)PTE2PA(*pte);
    }
    return 0;
}

uint64 vm_translate(uint64 va)
{
    return vm_translate_in(kernel_pagetable, va);
}

/* Copy across address spaces one page-fragment at a time. The kernel table
   is installed while this runs, so a physical address doubles as a usable
   pointer — that identity map is precisely what lets the kernel act as the
   middleman two isolated tasks now require. */
int vm_copy_across(pagetable_t dpt, uint64 dva,
                   pagetable_t spt, uint64 sva, uint64 len)
{
    while (len) {
        uint64 spa = vm_translate_in(spt, sva);
        uint64 dpa = vm_translate_in(dpt, dva);
        if (!spa || !dpa)
            return -1;                       /* unmapped on one side */

        uint64 n = PGSIZE - (sva % PGSIZE);
        uint64 m = PGSIZE - (dva % PGSIZE);
        if (m < n) n = m;
        if (n > len) n = len;

        memcpy((void *)dpa, (const void *)spa, (size_t)n);
        sva += n;
        dva += n;
        len -= n;
    }
    return 0;
}

int vm_load_segment(struct task *t, pagetable_t src, const struct vmload *seg)
{
    uint64 start = PGROUNDDOWN(seg->va);
    uint64 end   = PGROUNDUP(seg->va + seg->memsz);
    uint64 perm  = seg->perm & (PTE_R | PTE_W | PTE_X);
    if (!perm)
        perm = PTE_R;

    for (uint64 a = start; a < end; a += PGSIZE) {
        if (vm_translate_in(t->pt, a))
            continue;                 /* adjacent segments may share a page */
        void *p = pmm_alloc();        /* arrives zeroed, which is where .bss
                                         comes from: memsz beyond filesz is
                                         simply never written */
        if (!p)
            return -1;
        if (vm_map_at(t->pt, a, (uint64)p, PGSIZE, perm | PTE_U, 0) < 0)
            return -1;
    }

    /* The copy goes through physical addresses, so a read-only text segment
       is writable at this moment and not afterwards. */
    if (seg->filesz &&
        vm_copy_across(t->pt, seg->va, src, seg->src, seg->filesz) < 0)
        return -1;
    return 0;
}

/* ---- rendering a walk as text (for /proc/pagetable) ------------------ */

static int put(char *o, int n, const char *s)
{
    int l = (int)strlen(s);
    memcpy(o + n, s, (size_t)l);
    return n + l;
}

static int put_hex(char *o, int n, uint64 v)
{
    n = put(o, n, "0x");
    return n + xtoa(v, o + n);
}

static int put_perm(char *o, int n, pte_t pte)
{
    o[n++] = (pte & PTE_R) ? 'r' : '-';
    o[n++] = (pte & PTE_W) ? 'w' : '-';
    o[n++] = (pte & PTE_X) ? 'x' : '-';
    o[n++] = (pte & PTE_U) ? 'u' : '-';
    return n;
}

int vm_dump_walk(uint64 va, char *out, int cap)
{
    return vm_dump_walk_in(kernel_pagetable, va, out, cap);
}

int vm_dump_walk_in(pagetable_t pt, uint64 va, char *out, int cap)
{
    int n = 0;
    if (cap < 400)
        return 0;

    n = put(out, n, "va ");
    n = put_hex(out, n, va);
    n = put(out, n, "\n");

    for (int level = 2; level >= 0; level--) {
        pte_t *pte = &pt[PX(level, va)];

        n = put(out, n, "  L");
        n += utoa((unsigned long)level, out + n);
        n = put(out, n, " idx ");
        n += utoa((unsigned long)PX(level, va), out + n);
        n = put(out, n, " pte ");
        n = put_hex(out, n, *pte);

        if (!(*pte & PTE_V)) {
            n = put(out, n, "  invalid -> unmapped\n");
            return n;
        }
        if (PTE_IS_LEAF(*pte)) {
            uint64 psz = PGSIZE << (9 * level);
            n = put(out, n, "  leaf ");
            n = put_perm(out, n, *pte);
            n = put(out, n, " pa ");
            n = put_hex(out, n, PTE2PA(*pte) | (va & (psz - 1)));
            n = put(out, n, level == 0 ? " (4KiB)\n" :
                            level == 1 ? " (2MiB superpage)\n" : " (1GiB superpage)\n");
            return n;
        }
        n = put(out, n, "  -> table ");
        n = put_hex(out, n, PTE2PA(*pte));
        n = put(out, n, "\n");
        pt = (pagetable_t)PTE2PA(*pte);
    }
    return n;
}

/* ---- building the kernel address space ------------------------------- */

void vm_init(void)
{
    kernel_pagetable = pmm_alloc();

    /* RAM identity-mapped with 2 MiB superpages: 64 PTEs cover all 128 MiB,
       including the kernel image, the page arena and the FAT16 disk image.
       Identity mapping keeps physical pointers (page tables, IPC payloads)
       valid, which the rest of rvos still relies on. */
    vm_map_at(kernel_pagetable, RAM_BASE, RAM_BASE, RAM_TOP - RAM_BASE,
              PTE_R | PTE_W | PTE_X, 1);

    /* The UART deliberately gets a 4 KiB page, so at least one mapping
       exercises the full three-level walk (see /proc/pagetable). */
    vm_map_at(kernel_pagetable, UART_BASE_PA, UART_BASE_PA, PGSIZE,
              PTE_R | PTE_W, 0);

    /* CLINT, still 4 KiB pages: unused once the timer moves to Sstc, but
       mapping it keeps the machine-timer path available. */
    vm_map_at(kernel_pagetable, CLINT_BASE, CLINT_BASE, 0x10000,
              PTE_R | PTE_W, 0);

    kernel_satp = MAKE_SATP(kernel_pagetable);
    w_satp(kernel_satp);
    sfence_vma();               /* from here on every address is translated */
}

/* An address space containing only what a task legitimately needs:

     - the kernel image (text, rodata, data, bss), because kernel code and
       its data structures must stay reachable when a trap lands before the
       handler has switched to the kernel table;
     - the UART and CLINT.

   Deliberately absent is the free-page arena, which is where every task's
   stack is allocated from. That omission is the isolation: task A holds no
   translation for task B's stack, so the MMU refuses the access outright.
   The kernel image is mapped with 4 KiB pages rather than a superpage for
   exactly this reason — a 2 MiB leaf would spill past _end and hand over
   the start of the arena with it. */
pagetable_t vm_create_task_pt(void)
{
    pagetable_t pt = pmm_alloc();
    if (!pt)
        return 0;

    uint64 kend = PGROUNDUP((uint64)_end);
    vm_map_at(pt, RAM_BASE, RAM_BASE, kend - RAM_BASE,
              PTE_R | PTE_W | PTE_X, 0);
    vm_map_at(pt, UART_BASE_PA, UART_BASE_PA, PGSIZE, PTE_R | PTE_W, 0);
    vm_map_at(pt, CLINT_BASE, CLINT_BASE, 0x10000, PTE_R | PTE_W, 0);
    return pt;
}
