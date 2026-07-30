/* kmain.c — Stage 8: Sv39 paging.

   Entry is now smain(), reached by mret out of mstart(): everything below
   this point runs in supervisor mode with translation on, because M-mode
   ignores satp and paging simply does not exist there.

   The demo shows three things the earlier stages could not: a live page-table
   walk (/proc/pagetable), a superpage versus a 4 KiB mapping, and the MMU
   actually refusing a write to a read-only page. */
#include "uart.h"
#include "task.h"
#include "syscall.h"
#include "vfs.h"
#include "servers.h"
#include "pmm.h"
#include "vm.h"
#include "util.h"

/* An address nothing else uses, mapped read-only for the protection demo. */
#define RO_DEMO_VA 0x40000000UL

static void cat(const char *path, const char *label)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        kprintf("  cat %s: no such file\n", path);
        return;
    }
    char buf[1024];
    int n = vfs_read(fd, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = 0;
    kprintf("%s\n%s", label, buf);
    vfs_close(fd);
}

static void say(const char *what)
{
    int fd = vfs_open("/dev/console");
    if (fd < 0) {
        kprintf("  (/dev/console unreachable)\n");
        return;
    }
    vfs_write(fd, what, (int)strlen(what));
    vfs_close(fd);
}

/* ---- namespace demo: same code, private namespace -------------------- */
static void sandbox(void)
{
    uint64 m;
    sys_recv(&m);

    kprintf("\n--- sandbox (task %d): its own namespace ---------------\n",
            SANDBOX_TASK_ID);
    vfs_ns_clone();
    vfs_bind("/dev/console", NULL_TASK_ID);
    kprintf("$ vfs_ns_clone(); bind /dev/console -> null\n");
    kprintf("$ write(/dev/console, ...)   -- same call as the shell's\n");
    say("  THIS LINE SHOULD NEVER APPEAR\n");
    kprintf("  (nothing printed: that path now reaches null)\n");

    sys_send(SHELL_TASK_ID, 0);
    for (;;)
        yield();
}

/* ---- protection demo: the MMU says no -------------------------------- */
static void faulter(void)
{
    uint64 m;
    sys_recv(&m);

    kprintf("\n--- faulter (task %d): page permissions ----------------\n",
            FAULTER_TASK_ID);
    volatile char *p = (volatile char *)RO_DEMO_VA;

    kprintf("$ read  *(char*)0x%lx  -- page is mapped r--\n", RO_DEMO_VA);
    char c = *p;
    kprintf("  read returned 0x%x, fine\n", (int)c);

    kprintf("$ write *(char*)0x%lx  -- same page, no W bit\n", RO_DEMO_VA);
    *p = 0x42;                       /* store page fault: task is retired */

    kprintf("  UNREACHABLE: the store should have faulted\n");
    for (;;)
        yield();
}

/* ---- shell ------------------------------------------------------------ */
static void shell(void)
{
    kprintf("\n--- shell (task %d): the filesystem --------------------\n",
            SHELL_TASK_ID);
    cat("/", "$ cat /");
    say("  write(/dev/console) still routed to the console module\n");

    sys_send(SANDBOX_TASK_ID, 0);
    uint64 m;
    sys_recv(&m);

    kprintf("\n--- shell: paging -------------------------------------\n");
    cat("/proc/pagetable", "$ cat /proc/pagetable");

    sys_send(FAULTER_TASK_ID, 0);    /* it never replies: it gets retired */

    for (;;)
        yield();
}

/* ---- supervisor-mode entry ------------------------------------------- */
void smain(void)
{
    uart_init();
    kprintf("\n=============================================\n");
    kprintf("  rvos — educational RISC-V microkernel\n");
    kprintf("  stage 8: Sv39 paging, running in S-mode\n");
    kprintf("=============================================\n");

    pmm_init();
    kprintf("[boot] %d physical pages free\n", pmm_free_count());

    vm_init();                       /* translation is live after this line */
    kprintf("[boot] Sv39 on, satp=0x%lx\n", r_satp());

    /* One read-only page, for the faulter to bounce off. */
    void *ro = pmm_alloc();
    vm_map_at(kernel_pagetable, RO_DEMO_VA, (uint64)ro, PGSIZE, PTE_R, 0);
    sfence_vma();

    trap_init();
    timer_init();

    task_create("fs",      fs_server);        /* 0 */
    task_create("console", console_server);   /* 1 */
    task_create("proc",    proc_server);      /* 2 */
    task_create("null",    null_server);      /* 3 */
    task_create("shell",   shell);            /* 4 */
    task_create("sandbox", sandbox);          /* 5 */
    task_create("faulter", faulter);          /* 6 */

    vfs_bind("/",      FS_TASK_ID);
    vfs_bind("/dev/",  CONSOLE_TASK_ID);
    vfs_bind("/proc/", PROC_TASK_ID);

    kprintf("[boot] 4 servers, 3 apps; starting scheduler.\n");
    scheduler_start();
}
