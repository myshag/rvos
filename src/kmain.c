/* kmain.c — Stage 6: a dynamic namespace over the one interface.
   Three modules with nothing in common — a FAT16 filesystem, a UART console,
   and a view of kernel state — all answer open/read/write/ioctl/close, and
   which one serves a path is decided by a mount table that can be changed
   while the system runs. The shell below reaches a module that was
   unreachable a moment earlier, purely by binding it into the namespace. */
#include "uart.h"
#include "task.h"
#include "syscall.h"
#include "vfs.h"
#include "servers.h"
#include "util.h"

/* Everything the shell does goes through the interface; it never calls
   fat16_*, never touches a UART register, never reads the task table. */
static void cat(const char *path)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        kprintf("$ cat %s\n  cat: %s: no such file\n\n", path, path);
        return;
    }
    char buf[512];
    int n = vfs_read(fd, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = 0;
    kprintf("$ cat %s\n%s\n", path, buf);
    vfs_close(fd);
}

static void shell(void)
{
    kprintf("\n--- filesystem module -----------------------------------\n");
    cat("/");                       /* a directory read()s like anything else */
    cat("/README.TXT");

    int fd = vfs_open("/README.TXT");
    if (fd >= 0) {
        uint32 sz = 0;
        vfs_ioctl(fd, IOCTL_GETSIZE, (unsigned long)&sz);
        kprintf("$ ioctl(/README.TXT, GETSIZE) -> %u bytes (file not read)\n\n", sz);
        vfs_close(fd);
    }

    kprintf("--- console module (same interface, no disk) ------------\n");
    fd = vfs_open("/dev/console");
    const char *msg = "$ write(/dev/console) -> these bytes came back via IPC\n\n";
    vfs_write(fd, msg, (int)strlen(msg));
    vfs_close(fd);

    kprintf("--- dynamic namespace ----------------------------------\n");
    kprintf("$ cat /proc/tasks        (before binding anything there)\n");
    fd = vfs_open("/proc/tasks");
    kprintf("  -> %s\n\n", fd < 0 ? "fails: nothing serves that subtree yet"
                                  : "unexpectedly open");

    kprintf("$ bind /proc/ -> task %d      (system already running)\n\n", PROC_TASK_ID);
    vfs_bind("/proc/", PROC_TASK_ID);

    cat("/proc/tasks");
    cat("/proc/mounts");

    kprintf("[shell] one interface, three modules, namespace changed at\n"
            "        runtime — and the kernel never learned any of it.\n");

    for (;;)
        yield();
}

void kmain(void)
{
    uart_init();
    kprintf("\n=============================================\n");
    kprintf("  rvos — educational RISC-V microkernel\n");
    kprintf("  stage 6: dynamic namespace over one interface\n");
    kprintf("=============================================\n");

    trap_init();
    timer_init();

    /* IDs are creation order and must match servers.h. */
    task_create("fs",      fs_server);        /* 0 */
    task_create("console", console_server);   /* 1 */
    task_create("proc",    proc_server);      /* 2 */
    task_create("shell",   shell);            /* 3 */

    /* Initial namespace: filesystem as the catch-all, console carved out.
       /proc is deliberately left unbound — the shell binds it later. */
    vfs_bind("/",     FS_TASK_ID);
    vfs_bind("/dev/", CONSOLE_TASK_ID);

    kprintf("[boot] 3 servers + shell; namespace: / and /dev/\n");
    scheduler_start();
}
