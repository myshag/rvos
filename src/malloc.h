#pragma once
#include "syscall.h"

/* malloc.h — memory that is asked for rather than declared.

   Everything in this system that needed a buffer had one written into it at
   compile time: the loaders' scratch space for an ELF file, the filesystem
   server's copy of an open file, the editor's copy of the text. Those numbers
   are the reason `mc` stopped fitting and had to be given a larger cage
   rather than let out of it. A limit that is a static array is not a policy —
   it is an absence.

   The kernel's whole contribution is one call. `sbrk` moves the end of a
   window in the calling task's address space and answers with where it used
   to be; growing maps fresh pages, shrinking unmaps them and gives the frames
   back. Everything below this line is arithmetic on top of that, running
   unprivileged, in the same address space as the memory it hands out.

   **The state is at a fixed address, not in a variable, and that is the one
   interesting decision here.** User programs in this system share their text
   and not their data: `spawn` lives in the shared user text and is called by
   the shell, so a static variable of its own would be at an address mapped
   into some other task and not into the caller's. An allocator with a
   `static struct mheap *heap` would fault the first time the shell allocated.
   So the heap describes itself: its header is the first thing in the region
   it manages, at UHEAP_BASE, and the kernel maps that page when the task is
   created — a page arrives zeroed, and zero is what this reads as "nothing
   here yet". One copy of the code then works for whoever is running.

   The allocator itself is the plain one: a single free list kept in address
   order, first fit, split on the way out, coalesced with both neighbours on
   the way back in. When nothing fits it asks the kernel for whole pages, and
   when the last block reaches the break it gives whole pages back. It is not
   fast and does not pretend to be — there is no arena per size class, no
   thread cache and nothing to lock, because a task is single-threaded and
   its heap is its own. What it has instead is that you can read it. */

#define MHEAP_MAGIC 0x6d616c6c6f633031UL         /* "malloc01" */

#define MALIGN(x)   (((x) + 15UL) & ~15UL)

struct mblk {
    unsigned long size;         /* the whole block, this header included */
    struct mblk  *next;         /* only meaningful while it is free */
};

struct mheap {
    unsigned long magic;
    struct mblk  *free;         /* address-ordered, so coalescing is local */
    unsigned long top;          /* the break, as the allocator last left it */
};

#define MHDR MALIGN(sizeof(struct mblk))
#define MMIN (MHDR + 16UL)      /* smaller than this is not worth splitting */

static inline void m__copy(void *d, const void *s, unsigned long n)
{
    char *a = d; const char *b = s;
    while (n--) *a++ = *b++;
}

static inline void m__zero(void *d, unsigned long n)
{
    char *a = d;
    while (n--) *a++ = 0;
}

/* Put a block back on the list and join it to whatever it touches. Two
   neighbours, two tests: the list is in address order precisely so that they
   are the only two candidates. */
static inline void m__insert(struct mheap *h, struct mblk *b,
                             unsigned long size)
{
    b->size = size;
    struct mblk *prev = 0, *cur = h->free;
    while (cur && cur < b) { prev = cur; cur = cur->next; }
    b->next = cur;
    if (prev) prev->next = b; else h->free = b;

    if (cur && (char *)b + b->size == (char *)cur) {
        b->size += cur->size;
        b->next  = cur->next;
    }
    if (prev && (char *)prev + prev->size == (char *)b) {
        prev->size += b->size;
        prev->next  = b->next;
    }
}

/* Whole pages at the end of the heap are of no use to this program and every
   use to the next one — but only past a margin. Giving back the moment the
   last block is a page long means the next allocation asks for it straight
   back, and this is the one place in the allocator that costs a syscall.
   Sixty-four kilobytes of slack is a guess, and an honest one. */
#define MTRIM (16UL * PGSIZE)

static inline void m__trim(struct mheap *h)
{
    struct mblk *last = h->free;
    if (!last)
        return;
    while (last->next)
        last = last->next;
    if ((unsigned long)last + last->size != h->top)
        return;                         /* the tail is in use */

    unsigned long from = PGROUNDUP((unsigned long)last + MHDR);
    if (from < UHEAP_BASE + PGSIZE)
        from = UHEAP_BASE + PGSIZE;
    if (h->top <= from || h->top - from < MTRIM)
        return;

    unsigned long give = h->top - from;
    if (sys_sbrk(-(long)give) != (void *)-1) {
        h->top     = from;
        last->size = from - (unsigned long)last;
    }
}

static inline struct mheap *m__heap(void)
{
    struct mheap *h = (struct mheap *)UHEAP_BASE;
    if (h->magic != MHEAP_MAGIC) {
        h->magic = MHEAP_MAGIC;
        h->free  = 0;
        h->top   = UHEAP_BASE + PGSIZE;
        /* The rest of the page the kernel already gave us is the first thing
           on the free list, so the common small allocation costs no call. */
        unsigned long first = UHEAP_BASE + MALIGN(sizeof(struct mheap));
        m__insert(h, (struct mblk *)first, h->top - first);
    }
    return h;
}

static inline int m__grow(struct mheap *h, unsigned long need)
{
    unsigned long n = PGROUNDUP(need);
    void *p = sys_sbrk((long)n);
    if (p == (void *)-1)
        return -1;
    /* sbrk answers with the old break, which is where the new pages begin.
       Inserted and not trimmed: trimming here would hand back the very pages
       this call went to get, which is exactly the bug this comment replaces. */
    h->top = (unsigned long)p + n;
    m__insert(h, (struct mblk *)p, n);
    return 0;
}

static inline void *malloc(unsigned long n)
{
    if (!n)
        return 0;
    struct mheap *h = m__heap();
    unsigned long want = MALIGN(n + MHDR);
    if (want < n)                       /* the addition wrapped: absurd size */
        return 0;

    for (int pass = 0; pass < 2; pass++) {
        struct mblk *prev = 0;
        for (struct mblk *b = h->free; b; prev = b, b = b->next) {
            if (b->size < want)
                continue;
            if (b->size - want >= MMIN) {
                struct mblk *rest = (struct mblk *)((char *)b + want);
                rest->size = b->size - want;
                rest->next = b->next;
                if (prev) prev->next = rest; else h->free = rest;
                b->size = want;
            } else {
                if (prev) prev->next = b->next; else h->free = b->next;
            }
            return (char *)b + MHDR;
        }
        if (m__grow(h, want) < 0)
            break;
    }
    return 0;
}

static inline void free(void *p)
{
    if (!p)
        return;
    struct mheap *h = m__heap();
    struct mblk *b = (struct mblk *)((char *)p - MHDR);
    m__insert(h, b, b->size);
    m__trim(h);
}

static inline void *calloc(unsigned long n, unsigned long size)
{
    unsigned long total = n * size;
    if (n && total / n != size)
        return 0;
    void *p = malloc(total);
    if (p)
        m__zero(p, total);
    return p;
}

static inline void *realloc(void *p, unsigned long n)
{
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return 0;
    }
    struct mblk *b = (struct mblk *)((char *)p - MHDR);
    unsigned long have = b->size - MHDR;
    if (have >= n)
        return p;                       /* it already fits; say nothing */
    void *q = malloc(n);
    if (!q)
        return 0;
    m__copy(q, p, have);
    free(p);
    return q;
}

/* What this task has taken from the kernel, how much of it is idle, how many
   pieces that idle memory is in and how big the largest piece is. Not part of
   anybody's standard. It is here because a system that can now leak needs a
   way to be asked whether it is — and because the difference between `idle`
   and `largest` is the entire subject of fragmentation, which cannot be
   discussed without being measured. */
static inline void malloc_stat(unsigned long *taken, unsigned long *idle,
                               int *pieces, unsigned long *largest)
{
    struct mheap *h = m__heap();
    unsigned long f = 0, big = 0;
    int n = 0;
    for (struct mblk *b = h->free; b; b = b->next) {
        f += b->size;
        n++;
        if (b->size > big)
            big = b->size;
    }
    if (taken)   *taken   = h->top - UHEAP_BASE;
    if (idle)    *idle    = f;
    if (pieces)  *pieces  = n;
    if (largest) *largest = big;
}
