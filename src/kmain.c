/* kmain.c — Stage 9: address-space isolation.

   Every task now owns an Sv39 page table. They share the kernel image and the
   console, because kernel code must run when a trap lands, but each stack is
   private and the free-page arena is mapped nowhere. The FAT16 image goes
   into the filesystem server's space alone, so "only the fs module can reach
   the disk" stops being a convention and becomes something the hardware
   enforces — as the snooper task finds out.

   The visible cost is in IPC: a message can no longer be a pointer, since an
   address means nothing in another address space, so the kernel copies. */
#include "uart.h"
#include "task.h"
#include "syscall.h"
#include "vfs.h"
#include "servers.h"
#include "pmm.h"
#include "vm.h"
#include "util.h"

/* Read a whole file through the interface. It arrives in VFS_DATA_MAX-sized
   chunks now — each one a copy the kernel made between two address spaces. */
static void cat(const char *path, const char *label)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        kprintf("  cat %s: no such file\n", path);
        return;
    }
    kprintf("%s\n", label);
    for (;;) {
        char buf[VFS_DATA_MAX + 1];
        int n = vfs_read(fd, buf, VFS_DATA_MAX);
        if (n <= 0)
            break;
        buf[n] = 0;
        kprintf("%s", buf);
    }
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
    sys_recv(&m, (int)sizeof(m));

    kprintf("\n--- sandbox (task %d) ----------------------------------\n",
            SANDBOX_TASK_ID);
    vfs_ns_clone();
    vfs_bind("/dev/console", NULL_TASK_ID);
    kprintf("$ vfs_ns_clone(); bind /dev/console -> null\n");
    say("  THIS LINE SHOULD NEVER APPEAR\n");
    kprintf("  write(/dev/console) printed nothing: rebound to null\n");

    /* Same path, same code the shell ran — different answer, because the
       page table reported is whichever task is asking. */
    cat("/proc/pagetable", "\n$ cat /proc/pagetable  -- sandbox's own space");

    sys_send(SHELL_TASK_ID, &m, (int)sizeof(m));
    for (;;)
        yield();
}

/* ---- isolation demo: reach for another module's memory --------------- */
static void snooper(void)
{
    uint64 m;
    sys_recv(&m, (int)sizeof(m));

    kprintf("\n--- snooper (task %d) ----------------------------------\n",
            SNOOPER_TASK_ID);
    kprintf("$ read *(char*)0x%lx  -- FAT16 image, mapped only into fs\n",
            DISK_PA);
    volatile char *disk = (volatile char *)DISK_PA;
    char c = *disk;                  /* load page fault: task is retired */

    kprintf("  UNREACHABLE: read 0x%x — isolation failed\n", (int)c);
    for (;;)
        yield();
}

/* ---- shell ------------------------------------------------------------ */
static void shell(void)
{
    kprintf("\n--- shell (task %d) ------------------------------------\n",
            SHELL_TASK_ID);
    cat("/", "$ cat /                -- fs reaches the disk on our behalf");
    say("  write(/dev/console) routed to the console module\n");

    cat("/proc/pagetable", "\n$ cat /proc/pagetable  -- shell's own space");

    uint64 m = 0;
    sys_send(SANDBOX_TASK_ID, &m, (int)sizeof(m));
    sys_recv(&m, (int)sizeof(m));

    sys_send(SNOOPER_TASK_ID, &m, (int)sizeof(m));   /* it never replies */

    for (;;)
        yield();
}

/* Always runnable, so the scheduler never runs out of candidates — without
   it, retiring a faulting task while everyone else is blocked left nothing to
   switch to. */
static void idle(void)
{
    for (;;)
        yield();
}

/* ---- supervisor-mode entry ------------------------------------------- */
void smain(void)
{
    uart_init();
    kprintf("\n=============================================\n");
    kprintf("  rvos — educational RISC-V microkernel\n");
    kprintf("  stage 9: per-task address spaces\n");
    kprintf("=============================================\n");

    pmm_init();
    vm_init();
    kprintf("[boot] Sv39 on, kernel satp=0x%lx, %d pages free\n",
            r_satp(), pmm_free_count());

    trap_init();
    timer_init();

    struct task *fs = task_create("fs", fs_server);   /* 0 */
    task_create("console", console_server);           /* 1 */
    task_create("proc",    proc_server);              /* 2 */
    task_create("null",    null_server);              /* 3 */
    task_create("shell",   shell);                    /* 4 */
    task_create("sandbox", sandbox);                  /* 5 */
    task_create("snooper", snooper);                  /* 6 */
    task_create("idle",    idle);                     /* 7 */

    /* The disk goes into exactly one address space. Every other task holds no
       translation for it at all — which is what the snooper discovers. */
    vm_map_at(fs->pt, DISK_PA, DISK_PA, DISK_SIZE, PTE_R, 1);

    vfs_bind("/",      FS_TASK_ID);
    vfs_bind("/dev/",  CONSOLE_TASK_ID);
    vfs_bind("/proc/", PROC_TASK_ID);

    kprintf("[boot] 4 servers, 3 apps + idle, %d pages left; scheduling.\n",
            pmm_free_count());
    scheduler_start();
}
