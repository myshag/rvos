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

/* A run: one page carved into slots of a single size. Everything in it is
   the same size, so a slot needs no header of its own — where it came from
   is a property of the page, not of the object. That is the second saving
   after fragmentation: a 48-byte allocation costs 48 bytes here and 64 in a
   heap where every block carries a size and a link. */
struct mrun {
    struct mrun *next;          /* the next run of this class with room */
    void        *slots;         /* free slots, threaded through themselves */
    unsigned short cls, used;
};

#define MRUNHDR 32              /* MALIGN(sizeof(struct mrun)) */

/* The sizes small objects are rounded up to. Under 128 they step by 16, then
   by a quarter each time, so nothing is rounded up by more than about an
   eighth of itself. Above the last of them an allocation is on its own. */
#define NCLASS   16
#define MAXSMALL 512

static const unsigned short m__class[NCLASS] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512
};

struct mheap {
    unsigned long magic;
    struct mblk  *free;         /* address-ordered, so coalescing is local */
    unsigned long top;          /* the break, as the allocator last left it */
    struct mrun  *runs[NCLASS]; /* runs with a free slot, one list per class */
    unsigned char *dir;         /* one byte a page: 0 = large, else class + 1 */
    unsigned long  dirlen;
    unsigned long  run_pages;   /* how many pages are runs, full ones included */
};

struct mstat {
    unsigned long taken;        /* from the kernel */
    unsigned long idle;         /* on the large free list */
    unsigned long largest;      /* the biggest piece of that */
    int           pieces;
    unsigned long run_bytes;    /* pages given over to runs */
    unsigned long run_free;     /* unused slots inside them */
    int           runs;
};

#define MHDR MALIGN(sizeof(struct mblk))
#define MMIN (MHDR + 16UL)      /* smaller than this is not worth splitting */

/* The two that the run machinery below needs before they are written: a run
   page is an ordinary block, so making one and giving one up go through the
   ordinary calls. */
static inline void  free(void *p);
static inline void *realloc(void *p, unsigned long n);

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
        h->magic  = MHEAP_MAGIC;
        h->free   = 0;
        h->top    = UHEAP_BASE + PGSIZE;
        h->dir       = 0;
        h->dirlen    = 0;
        h->run_pages = 0;
        for (int i = 0; i < NCLASS; i++)
            h->runs[i] = 0;
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

/* The large path, unchanged: first fit over one address-ordered list. */
static inline void *m__big(struct mheap *h, unsigned long n)
{
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

/* One page, page-aligned, out of the same pool as everything else.

   A run has to start on a page boundary because that is how a pointer into it
   is turned back into the run it belongs to: mask off the low twelve bits.
   And unlike every other allocation it carries no block header in front of
   it — the run's own header is inside the page, and the page is given back by
   its address and its size, both of which are known without one. That is not
   a saving of sixteen bytes; it is what lets a page be carved from a block
   that is already page-aligned. Requiring a header first meant a page could
   never be taken from the front of a fresh region, so every run cost two
   pages and half the heap sat in unusable four-kilobyte crumbs. */
static inline void *m__page(struct mheap *h)
{
    for (int pass = 0; pass < 2; pass++) {
        struct mblk *prev = 0;
        for (struct mblk *b = h->free; b; prev = b, b = b->next) {
            unsigned long start = (unsigned long)b;
            unsigned long end   = start + b->size;
            unsigned long page  = PGROUNDUP(start);
            unsigned long lead  = page - start;
            if (lead && lead < MMIN) {          /* a crumb is not a block */
                page += PGSIZE;
                lead  = page - start;
            }
            if (page + PGSIZE > end)
                continue;
            unsigned long tail = end - (page + PGSIZE);
            if (tail && tail < MMIN)
                continue;                       /* would strand a crumb */

            struct mblk *t = 0;
            if (tail) {
                t = (struct mblk *)(page + PGSIZE);
                t->size = tail;
                t->next = b->next;
            }
            if (lead) {
                b->size = lead;
                if (t) b->next = t;
            } else {
                struct mblk *rest = t ? t : b->next;
                if (prev) prev->next = rest; else h->free = rest;
            }
            return (void *)page;
        }
        /* The break is page-aligned and grows by page multiples, so one page
           asked for is one page-aligned block with nothing left over. */
        if (m__grow(h, PGSIZE) < 0)
            break;
    }
    return 0;
}

/* The page map: one byte for every page of heap, saying which class of run
   lives there, or nothing. tcmalloc has the same thing under the same name
   and for the same reason — given a pointer and nothing else, free() has to
   know what it is looking at. */
static inline int m__dir_cover(struct mheap *h, unsigned long page)
{
    unsigned long idx = (page - UHEAP_BASE) / PGSIZE;
    if (idx < h->dirlen)
        return (int)idx;

    /* The map is taken from the large path on purpose, and its smallest size
       is chosen to be past the last class for the same reason: a map wanted
       through a run would need a run, which would need a map. That circle
       cost an afternoon — it does not fail an allocation, it recurses until
       the server's stack runs off the bottom of its last page, which is a
       fault nowhere near the mistake. */
    unsigned long want = h->dirlen ? h->dirlen * 2 : 4 * MAXSMALL;
    while (want <= idx)
        want *= 2;
    unsigned char *d = m__big(h, want);
    if (!d)
        return -1;
    for (unsigned long i = 0; i < want; i++)
        d[i] = i < h->dirlen ? h->dir[i] : 0;
    if (h->dir)
        free(h->dir);
    h->dir    = d;
    h->dirlen = want;
    return (int)idx;
}

static inline struct mrun *m__newrun(struct mheap *h, int c)
{
    void *page = m__page(h);
    if (!page)
        return 0;
    int idx = m__dir_cover(h, (unsigned long)page);
    if (idx < 0) {
        m__insert(h, (struct mblk *)page, PGSIZE);   /* it has no header */
        return 0;
    }
    h->dir[idx] = (unsigned char)(c + 1);

    struct mrun *r = (struct mrun *)page;
    r->next  = h->runs[c];
    r->cls   = (unsigned short)c;
    r->used  = 0;
    r->slots = 0;
    unsigned long sz = m__class[c];
    for (char *p = (char *)page + PGSIZE - sz;
         p >= (char *)page + MRUNHDR; p -= sz) {
        *(void **)p = r->slots;
        r->slots = p;
    }
    h->runs[c] = r;
    h->run_pages++;
    return r;
}

static inline void *malloc(unsigned long n)
{
    if (!n)
        return 0;
    struct mheap *h = m__heap();
    if (n > MAXSMALL)
        return m__big(h, n);

    int c = 0;
    while (m__class[c] < n)
        c++;
    struct mrun *r = h->runs[c];
    if (!r && !(r = m__newrun(h, c)))
        return 0;

    void *p = r->slots;
    r->slots = *(void **)p;
    r->used++;
    if (!r->slots)                      /* full: off the list of runs with room */
        h->runs[c] = r->next;
    return p;
}

static inline void free(void *p)
{
    if (!p)
        return;
    struct mheap *h = m__heap();
    unsigned long page = (unsigned long)p & ~(unsigned long)(PGSIZE - 1);
    unsigned long idx  = (page - UHEAP_BASE) / PGSIZE;

    if (idx < h->dirlen && h->dir[idx]) {
        struct mrun *r = (struct mrun *)page;
        if (!r->slots) {                /* it was full, so it is off the list */
            r->next = h->runs[r->cls];
            h->runs[r->cls] = r;
        }
        *(void **)p = r->slots;
        r->slots = p;
        if (--r->used)
            return;

        /* Empty. The page goes back to the one pool, where it can coalesce
           with its neighbours and be trimmed — which is the whole reason runs
           are taken from there rather than from a pool of their own. */
        struct mrun **pp = &h->runs[r->cls];
        while (*pp && *pp != r)
            pp = &(*pp)->next;
        if (*pp)
            *pp = r->next;
        h->dir[idx] = 0;
        h->run_pages--;
        m__insert(h, (struct mblk *)page, PGSIZE);
        m__trim(h);
        return;
    }

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
    struct mheap *h = m__heap();
    unsigned long page = (unsigned long)p & ~(unsigned long)(PGSIZE - 1);
    unsigned long idx  = (page - UHEAP_BASE) / PGSIZE;
    unsigned long have;
    if (idx < h->dirlen && h->dir[idx])
        have = m__class[h->dir[idx] - 1];
    else
        have = ((struct mblk *)((char *)p - MHDR))->size - MHDR;
    if (have >= n)
        return p;                       /* it already fits; say nothing */
    void *q = malloc(n);
    if (!q)
        return 0;
    m__copy(q, p, have);
    free(p);
    return q;
}

/* What this task has taken from the kernel, how much of it is idle, in how
   many pieces, and how big the largest piece is — the difference between the
   second and the fourth being the entire subject of fragmentation, which
   cannot be discussed without being measured. Plus what the runs hold, since
   memory inside a half-used run is idle in a different way: it is reserved
   for one size and useless to any other. Not part of anybody's standard. */
static inline void malloc_stat(struct mstat *st)
{
    struct mheap *h = m__heap();
    st->taken = h->top - UHEAP_BASE;
    st->idle = st->largest = 0;
    st->pieces = 0;
    for (struct mblk *b = h->free; b; b = b->next) {
        st->idle += b->size;
        st->pieces++;
        if (b->size > st->largest)
            st->largest = b->size;
    }
    /* Only runs with room are on a list, which is exactly the ones that have
       anything spare — a full run contributes nothing to the second number,
       so counting pages separately gets both right. */
    st->runs      = (int)h->run_pages;
    st->run_bytes = h->run_pages * PGSIZE;
    st->run_free  = 0;
    for (int c = 0; c < NCLASS; c++)
        for (struct mrun *r = h->runs[c]; r; r = r->next) {
            unsigned long slots = (PGSIZE - MRUNHDR) / m__class[c];
            st->run_free += (slots - r->used) * m__class[c];
        }
}
