/* task.c — fixed table of tasks with a preemptive round-robin scheduler.
   Each task owns an Sv39 address space: the kernel image and MMIO are shared
   (kernel code has to run when a trap lands), but the stack is private and
   the free-page arena is not mapped at all, so one task holds no translation
   for another's memory. Isolation is why IPC copies rather than sharing. */
#include "task.h"
#include "uart.h"
#include "syscall.h"
#include "vfs.h"
#include "vm.h"
#include "pmm.h"
#include "elf.h"

struct task tasks[NTASK];
struct task *current;
static struct task *alloc_slot(void);

extern void _ret_to_task(void) __attribute__((noreturn));

/* The first page of the heap, mapped at birth like the stack. The allocator
   keeps its state at the front of it and nowhere else, which is what lets a
   single copy of malloc — sitting in the shared user text — run in a task
   whose private data it cannot reach. A page arrives zeroed, and zero is what
   the allocator reads as "not initialised yet".

   The cost is one page per task whether it allocates or not; the price of not
   paying it is a syscall on every malloc to ask whether the heap exists. */
static void map_heap_page(struct task *t, uint64 perm)
{
    void *p = pmm_alloc();
    if (!p)
        return;
    vm_map_at(t->pt, UHEAP_BASE, (uint64)p, PGSIZE, perm, 0);
    t->brk = UHEAP_BASE + PGSIZE;
}

struct task *task_create(const char *name, void (*entry)(void))
{
    struct task *t = alloc_slot();
    if (!t)
        return 0;
    for (int i = 0; i < 32; i++)
        t->ctx.x[i] = 0;

    /* Its own address space, and a stack that exists only inside it. Every
       task's stack is at USTACK_TOP, yet on different physical pages, so the
       identical address means something different depending on who asks. */
    t->pt = vm_create_task_pt();
    for (int i = 0; i < USTACK_PAGES; i++) {
        void *p = pmm_alloc();
        vm_map_at(t->pt, USTACK_TOP - (uint64)(i + 1) * PGSIZE,
                  (uint64)p, PGSIZE, PTE_R | PTE_W, 0);
    }

    map_heap_page(t, PTE_R | PTE_W);

    t->ctx.x[2]   = USTACK_TOP;                        /* sp -> stack top */
    t->ctx.satp   = MAKE_SATP(t->pt);
    t->ctx.epc    = (uint64)entry;
    /* sret returns to S-mode (SPP=1) with interrupts enabled (SPIE -> SIE) */
    t->ctx.status = SSTATUS_SPP | SSTATUS_SPIE;
    t->state = T_RUNNABLE;
    t->name  = name;
    t->ns    = vfs_root_ns();   /* inherit the shared view until it clones */
    return t;
}

/* Round-robin: from the task after `current`, pick the next RUNNABLE one. */
/* An address space with no program in it yet: kernel image mapped U-less for
   the trap path, a private stack, and otherwise empty. It stays T_UNUSED —
   not a candidate for the scheduler — until SYS_START gives it an entry
   point. That is what lets a loader build it incrementally. */
/* A free slot is one that was never used or has been reclaimed: state UNUSED
   with no address space. A task under construction already has a page table,
   so it will not be handed out twice. */
static struct task *alloc_slot(void)
{
    for (int i = 0; i < NTASK; i++)
        if (tasks[i].state == T_UNUSED && !tasks[i].pt) {
            /* A slot handed out again is a *different* task, and its id has
               to say so. The generation starts at 0, so the tasks created at
               boot keep the ids servers.h names them by. */
            tasks[i].id = i | (tasks[i].gen << 8);
            return &tasks[i];
        }
    return 0;
}

struct task *task_by_id(int id)
{
    int i = TASK_SLOT(id);
    if (i < 0 || i >= NTASK)
        return 0;
    if (tasks[i].state == T_UNUSED && !tasks[i].pt)
        return 0;                      /* free: nobody at all */
    if (tasks[i].id != id)
        return 0;                      /* reused: somebody else entirely */
    return &tasks[i];
}

/* Release a task: its private pages and page tables go back to the allocator
   and the slot becomes available again. Anything still queued to send to it
   is dropped — a sender blocked on a dead receiver would wait forever. */
void task_retire(struct task *t)
{
    for (struct task *s = t->wait_sender; s; ) {
        struct task *nxt = s->send_next;
        s->ctx.x[10] = (uint64)-1;      /* its send() fails rather than hangs */
        s->state     = T_RUNNABLE;
        s->send_next = 0;
        s = nxt;
    }
    t->wait_sender  = 0;
    t->waiting_recv = 0;
    /* Anyone waiting for this task has been waiting for exactly this. Done
       before the id is retired, since that is what they named. */
    for (int i = 0; i < NTASK; i++) {
        if (tasks[i].state == T_BLOCKED && tasks[i].wait_for == t->id) {
            tasks[i].wait_for  = 0;
            tasks[i].ctx.x[10] = 0;
            tasks[i].state     = T_RUNNABLE;
        }
        /* And anyone blocked waiting to hear from it in particular. A closed
           receive is the one wait that can be aimed at a task that then
           disappears, and it has to fail rather than last for ever. */
        if (tasks[i].state == T_BLOCKED && tasks[i].waiting_recv &&
            tasks[i].recv_closed && tasks[i].recv_from == t->id) {
            tasks[i].waiting_recv = 0;
            tasks[i].recv_closed  = 0;
            tasks[i].ctx.x[10]    = (uint64)-1;
            tasks[i].state        = T_RUNNABLE;
        }
    }

    vm_free_task(t);                    /* also clears t->pt */
    t->state = T_UNUSED;
    t->gen++;                           /* whatever still names this id is
                                           naming a task that no longer is */
    /* Its view of the tree may have been private, and may now be nobody's.
       This is the only moment a namespace can become unreferenced, so it is
       the only place worth looking. */
    vfs_ns_gc();
}

struct task *task_new_empty(const char *name)
{
    struct task *t = alloc_slot();
    if (!t)
        return 0;

    for (int i = 0; i < 32; i++)
        t->ctx.x[i] = 0;
    t->pt = vm_create_task_pt();
    if (!t->pt)
        return 0;
    for (int i = 0; i < USTACK_PAGES; i++) {
        void *p = pmm_alloc();
        vm_map_at(t->pt, USTACK_TOP - (uint64)(i + 1) * PGSIZE,
                  (uint64)p, PGSIZE, PTE_R | PTE_W | PTE_U, 0);
    }
    map_heap_page(t, PTE_R | PTE_W | PTE_U);
    t->ctx.satp = MAKE_SATP(t->pt);
    int k = 0;
    for (; k < 15 && name[k]; k++)
        t->namebuf[k] = name[k];
    t->namebuf[k] = 0;
    t->name     = t->namebuf;      /* the caller's buffer will not outlive us */
    /* Plan 9's rule: a child sees what its parent sees. It matters as soon as
       a parent has bent its own view — a shell reached over TCP binds
       /dev/console to the network, and the program it starts inherits that
       and writes to the connection without knowing it has. Before this, a new
       task got the root namespace and no arrangement its parent made could
       reach it. vfs_ns_clone() is still there for a child that wants to
       diverge. */
    t->ns       = current ? current->ns : vfs_root_ns();
    t->state    = T_UNUSED;            /* not runnable until started */
    return t;
}

/* A task that runs with the U bit set. Same machinery, two differences: its
   pages carry PTE_U, and sstatus.SPP is left clear so sret drops to user
   mode. The kernel image stays mapped — the trap vector has to be reachable
   — but without PTE_U, which is what puts the kernel out of its reach. */
struct task *task_create_user(const char *name, void (*entry)(void),
                              void *data_start, void *data_end)
{
    extern char __utext_start[], __utext_end[];

    struct task *t = task_create(name, entry);

    /* Shared, like a C library: every user program executes the same text. */
    vm_map_at(t->pt, (uint64)__utext_start, (uint64)__utext_start,
              (uint64)(__utext_end - __utext_start),
              PTE_R | PTE_X | PTE_U, 0);

    /* Private: only this program's own writable state, so one server cannot
       reach another's buffers even though they share their code. */
    if (data_end > data_start)
        vm_map_at(t->pt, (uint64)data_start, (uint64)data_start,
                  (uint64)((char *)data_end - (char *)data_start),
                  PTE_R | PTE_W | PTE_U, 0);

    for (int i = 0; i < USTACK_PAGES; i++) {
        uint64 va = USTACK_TOP - (uint64)(i + 1) * PGSIZE;
        uint64 pa = vm_translate_in(t->pt, va);
        vm_map_at(t->pt, va, pa, PGSIZE, PTE_R | PTE_W | PTE_U, 0);
    }
    vm_map_at(t->pt, UHEAP_BASE, vm_translate_in(t->pt, UHEAP_BASE),
              PGSIZE, PTE_R | PTE_W | PTE_U, 0);

    t->ctx.status = SSTATUS_SPIE;      /* SPP = 0: sret lands in U-mode */
    return t;
}

void schedule(void)
{
    int start = current ? current->id : -1;

    if (current && current->state == T_RUNNING)
        current->state = T_RUNNABLE;

    for (int i = 1; i <= NTASK; i++) {
        struct task *t = &tasks[(start + i) % NTASK];
        if (t->state == T_RUNNABLE) {
            t->state = T_RUNNING;
            current = t;
            return;
        }
    }
    /* The loop above also reconsiders `current` itself (i == NTASK), so a task
       that just blocked or was retired is not picked. With an always-runnable
       idle task this point is unreachable; if it is ever reached, park rather
       than resurrecting a dead task — doing that turned a page fault into an
       endless refault loop. */
    for (;;)
        __asm__ volatile("wfi");
}

void yield(void)
{
    sys_yield();        /* ecall -> _mtrap saves context -> schedule() */
}

/* Pull a NUL-terminated string out of the caller's address space, one byte
   at a time so we never read past the end of its buffer. */
static void copy_string_in(uint64 va, char *dst, int cap)
{
    int i = 0;
    for (; i < cap - 1; i++) {
        uint64 pa = vm_translate_in(current->pt, va + (uint64)i);
        if (!pa)
            break;
        dst[i] = *(char *)pa;
        if (!dst[i])
            break;
    }
    dst[i] = 0;
}

/* Dispatched from trap.c on an S-mode ecall. sepc has already been advanced
   past the ecall; args live in the saved context (a0=x[10], a1=x[11], ...). */
void syscall_dispatch(uint64 num)
{
    switch (num) {
    case SYS_YIELD:
        schedule();
        break;
    case SYS_PUTC:
        uart_putc((char)current->ctx.x[10]);
        break;
    case SYS_RESOLVE: {
        char kpath[VFS_PATH_MAX], kout[VFS_PATH_MAX];
        copy_string_in(current->ctx.x[10], kpath, VFS_PATH_MAX);
        int srv = vfs_resolve(kpath, kout, VFS_PATH_MAX,
                              (int)current->ctx.x[13]);
        uint64 out = current->ctx.x[11];
        int    cap = (int)current->ctx.x[12];
        if (srv >= 0 && out) {
            int n = 0;
            while (kout[n])
                n++;
            n++;                       /* the terminator travels too */
            if (n > cap)
                n = cap;
            if (n > 0)
                vm_copy_across(current->pt, out, kernel_pagetable,
                               (uint64)kout, (uint64)n);
        }
        current->ctx.x[10] = (uint64)(long)srv;
        break;
    }
    case SYS_TASKINFO: {
        int idx = (int)current->ctx.x[10];
        uint64 out = current->ctx.x[11];
        if (idx < 0 || idx >= NTASK || tasks[idx].state == T_UNUSED) {
            current->ctx.x[10] = (uint64)-1;
            break;
        }
        struct taskinfo ti;
        ti.id         = tasks[idx].id;
        ti.state      = (int)tasks[idx].state;
        ti.is_current = (&tasks[idx] == current);
        int k = 0;
        for (; k < 15 && tasks[idx].name[k]; k++)
            ti.name[k] = tasks[idx].name[k];
        ti.name[k] = 0;
        vm_copy_across(current->pt, out, kernel_pagetable,
                       (uint64)&ti, sizeof(ti));
        current->ctx.x[10] = 0;
        break;
    }
    case SYS_MOUNTS: {
        static char kbuf[512];
        int    tid = (int)current->ctx.x[10];
        uint64 out = current->ctx.x[11];
        int    cap = (int)current->ctx.x[12];
        int n = vfs_dump_mounts_of(tid, kbuf, (int)sizeof(kbuf));
        if (n > cap) n = cap;
        if (n > 0)
            vm_copy_across(current->pt, out, kernel_pagetable,
                           (uint64)kbuf, (uint64)n);
        current->ctx.x[10] = (uint64)n;
        break;
    }
    case SYS_MEMINFO: {
        int info[2] = { pmm_free_count(), pmm_total_count() };
        vm_copy_across(current->pt, current->ctx.x[10], kernel_pagetable,
                       (uint64)info, sizeof(info));
        current->ctx.x[10] = 0;
        break;
    }
    case SYS_UNMOUNT: {
        char kp[VFS_PREFIX_MAX];
        copy_string_in(current->ctx.x[10], kp, VFS_PREFIX_MAX);
        current->ctx.x[10] = (uint64)(long)vfs_unmount(kp);
        break;
    }
    case SYS_MOUNT: {
        char kp[VFS_PREFIX_MAX];
        copy_string_in(current->ctx.x[10], kp, VFS_PREFIX_MAX);
        current->ctx.x[10] = (uint64)(long)
            vfs_mount(kp, (int)current->ctx.x[11], (int)current->ctx.x[12]);
        break;
    }
    case SYS_BIND: {
        char kold[VFS_PATH_MAX], knew[VFS_PREFIX_MAX];
        copy_string_in(current->ctx.x[10], kold, VFS_PATH_MAX);
        copy_string_in(current->ctx.x[11], knew, VFS_PREFIX_MAX);
        current->ctx.x[10] = (uint64)(long)
            vfs_bind(kold, knew, (int)current->ctx.x[12]);
        break;
    }
    case SYS_NSCLONE:
        current->ctx.x[10] = (uint64)(long)vfs_ns_clone();
        break;
    /* ---- building another task, one segment at a time ---------------- */
    case SYS_WAIT: {
        int tid = (int)current->ctx.x[10];
        if (!task_by_id(tid)) {
            current->ctx.x[10] = 0;     /* already gone */
            break;
        }
        current->wait_for = tid;
        current->state    = T_BLOCKED;
        schedule();
        break;
    }
    case SYS_ALIVE:
        current->ctx.x[10] = task_by_id((int)current->ctx.x[10]) ? 1 : 0;
        break;
    case SYS_ALARM: {
        int ms = (int)current->ctx.x[10];
        /* QEMU's time base is 10 MHz, so a millisecond is 10000 ticks. The
           timer interrupt runs at ~50 ms, which bounds the resolution. */
        current->alarm_at = ms > 0 ? r_time() + (uint64)ms * 10000UL : 0;
        current->ctx.x[10] = 0;
        break;
    }
    case SYS_DMA_ALLOC: {
        void *p = pmm_alloc();
        if (!p) {
            current->ctx.x[10] = (uint64)-1;
            break;
        }
        if (!current->dma_next)
            current->dma_next = DMA_BASE;
        struct dmapage d;
        d.va = current->dma_next;
        d.pa = (uint64)p;
        vm_map_at(current->pt, d.va, d.pa, PGSIZE,
                  PTE_R | PTE_W | PTE_U, 0);
        current->dma_next += PGSIZE;
        sfence_vma();                  /* the mapping must be visible at once */
        vm_copy_across(current->pt, current->ctx.x[10],
                       kernel_pagetable, (uint64)&d, sizeof(d));
        current->ctx.x[10] = 0;
        break;
    }
    case SYS_IRQ_REG:
        current->ctx.x[10] =
            (uint64)(long)irq_register((int)current->ctx.x[10], current);
        break;
    case SYS_IRQ_ACK:
        current->ctx.x[10] = (uint64)(long)irq_ack((int)current->ctx.x[10]);
        break;
    case SYS_EXIT:
        task_retire(current);
        schedule();
        break;
    /* Grow or shrink this task's heap, and answer with where it used to end.
       The whole of malloc is above this line; the kernel's part is pages. */
    case SYS_SBRK: {
        long d = (long)current->ctx.x[10];
        if (!current->brk)
            current->brk = UHEAP_BASE;
        uint64 old = current->brk;
        uint64 want = old + (uint64)d;

        if (d > 0) {
            if (want > UHEAP_TOP || want < old) {
                current->ctx.x[10] = (uint64)-1;
                break;
            }
            uint64 a = PGROUNDUP(old);
            for (; a < PGROUNDUP(want); a += PGSIZE) {
                void *p = pmm_alloc();
                if (!p)
                    break;
                if (vm_map_at(current->pt, a, (uint64)p, PGSIZE,
                              PTE_R | PTE_W | PTE_U, 0) < 0) {
                    pmm_free(p);
                    break;
                }
            }
            if (a < PGROUNDUP(want)) {
                /* Out of memory partway: give back what this call took, so a
                   failed sbrk leaves the address space exactly as it was. */
                for (uint64 b = PGROUNDUP(old); b < a; b += PGSIZE)
                    vm_unmap_page(current->pt, b);
                sfence_vma();
                current->ctx.x[10] = (uint64)-1;
                break;
            }
        } else if (d < 0) {
            if (want < UHEAP_BASE + PGSIZE)
                want = UHEAP_BASE + PGSIZE;   /* the first page is never given back */
            for (uint64 a = PGROUNDUP(want); a < PGROUNDUP(old); a += PGSIZE)
                vm_unmap_page(current->pt, a);
            /* A mapping that has gone away may still be in the TLB, and this
               task is about to run again with the same satp. */
            sfence_vma();
        }
        current->brk = want;
        current->ctx.x[10] = old;
        break;
    }
    case SYS_NEWTASK: {
        char nm[16];
        copy_string_in(current->ctx.x[10], nm, sizeof(nm));
        struct task *t = task_new_empty(nm);
        current->ctx.x[10] = t ? (uint64)t->id : (uint64)-1;
        break;
    }
    case SYS_VMLOAD: {
        struct task *t = task_by_id((int)current->ctx.x[10]);
        struct vmload seg;
        if (!t || t == &tasks[0] || t->state != T_UNUSED || !t->pt) {
            current->ctx.x[10] = (uint64)-1;
            break;
        }
        vm_copy_across(kernel_pagetable, (uint64)&seg,
                       current->pt, current->ctx.x[11], sizeof(seg));
        current->ctx.x[10] =
            (uint64)(long)vm_load_segment(t, current->pt, &seg);
        break;
    }
    case SYS_START: {
        struct task *t = task_by_id((int)current->ctx.x[10]);
        if (!t || t == &tasks[0] || t->state != T_UNUSED || !t->pt) {
            current->ctx.x[10] = (uint64)-1;
            break;
        }
        struct startinfo si;
        vm_copy_across(kernel_pagetable, (uint64)&si,
                       current->pt, current->ctx.x[11], sizeof(si));
        t->ctx.epc    = si.entry;
        t->ctx.x[2]   = si.sp ? si.sp : USTACK_TOP;   /* sp */
        t->ctx.x[10]  = si.a0;                        /* argc */
        t->ctx.x[11]  = si.a1;                        /* argv */
        t->ctx.status = SSTATUS_SPIE;      /* SPP = 0: it starts in user mode */
        t->state      = T_RUNNABLE;
        current->ctx.x[10] = 0;
        break;
    }
    case SYS_PGDUMP: {
        /* Only the kernel can do this: it runs with the kernel table
           installed, which is the one address space that still reaches the
           page-table pages themselves. */
        static char kbuf[512];
        int    tid = (int)current->ctx.x[10];
        uint64 va  = current->ctx.x[11];
        uint64 out = current->ctx.x[12];
        int    cap = (int)current->ctx.x[13];
        pagetable_t pt = (tid >= 0 && tid < NTASK && tasks[tid].pt)
                         ? tasks[tid].pt : kernel_pagetable;
        int n = vm_dump_walk_in(pt, va, kbuf, (int)sizeof(kbuf));
        if (n > cap) n = cap;
        if (n > 0)
            vm_copy_across(current->pt, out,
                           kernel_pagetable, (uint64)kbuf, (uint64)n);
        current->ctx.x[10] = (uint64)n;
        break;
    }
    default:
        if (!ipc_syscall(num))
            kprintf("[warn] task %s: bad syscall %ld\n", current->name, num);
        break;
    }
}

void scheduler_start(void)
{
    current = 0;
    schedule();
    _ret_to_task();
}
