/* kmain.c — Stage 7: per-task namespaces.
   Four modules answer the same open/read/write/ioctl/close interface, and
   what a path *means* is a property of the task doing the asking. The shell
   and the sandbox below run identical code against "/dev/console" and get
   different modules, because the sandbox rebound that path inside its own
   private namespace. Nothing was patched, nothing was configured in either
   program — only the namespace changed. */
#include "uart.h"
#include "task.h"
#include "syscall.h"
#include "vfs.h"
#include "servers.h"
#include "util.h"

/* Both tasks use this; it goes through the interface and nothing else. */
static void cat(const char *path, const char *label)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        kprintf("  cat %s: no such file\n", path);
        return;
    }
    char buf[512];
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

/* ---- the sandboxed task: same code, private namespace ---------------- */
static void sandbox(void)
{
    uint64 m;
    sys_recv(&m);                       /* wait for the shell's go-ahead */

    kprintf("\n--- sandbox (task %d) ----------------------------------\n",
            SANDBOX_TASK_ID);
    kprintf("$ vfs_ns_clone()            -- take a private namespace\n");
    vfs_ns_clone();

    kprintf("$ bind /dev/console -> null (task %d)\n\n", NULL_TASK_ID);
    vfs_bind("/dev/console", NULL_TASK_ID);

    cat("/proc/mounts", "$ cat /proc/mounts          -- sandbox's own view");

    kprintf("\n$ write(/dev/console, \"...\")  -- identical call to the shell's\n");
    say("  THIS LINE SHOULD NEVER APPEAR\n");
    kprintf("  (nothing printed: the path now reaches null)\n");

    sys_send(SHELL_TASK_ID, 0);         /* hand control back */
    for (;;)
        yield();
}

/* ---- the shell ------------------------------------------------------- */
static void shell(void)
{
    kprintf("\n--- shell (task %d) ------------------------------------\n",
            SHELL_TASK_ID);
    cat("/", "$ cat /                     -- a directory read()s like a file");
    cat("/proc/mounts", "\n$ cat /proc/mounts          -- shell's view");

    kprintf("\n$ write(/dev/console, \"...\")\n");
    say("  visible: routed to the console module\n");

    sys_send(SANDBOX_TASK_ID, 0);       /* let the sandbox run its half */
    uint64 m;
    sys_recv(&m);                       /* ...and wait for it to finish */

    kprintf("\n--- back in the shell ----------------------------------\n");
    kprintf("$ write(/dev/console, \"...\")  -- unchanged by the sandbox\n");
    say("  still visible: the shell's namespace was never touched\n");

    cat("/proc/tasks", "\n$ cat /proc/tasks");

    kprintf("\n[shell] one interface, four modules, and a path that means\n"
            "        different things to different tasks.\n");
    for (;;)
        yield();
}

void kmain(void)
{
    uart_init();
    kprintf("\n=============================================\n");
    kprintf("  rvos — educational RISC-V microkernel\n");
    kprintf("  stage 7: per-task namespaces\n");
    kprintf("=============================================\n");

    trap_init();
    timer_init();

    /* Creation order defines the IDs in servers.h. */
    task_create("fs",      fs_server);        /* 0 */
    task_create("console", console_server);   /* 1 */
    task_create("proc",    proc_server);      /* 2 */
    task_create("null",    null_server);      /* 3 */
    task_create("shell",   shell);            /* 4 */
    task_create("sandbox", sandbox);          /* 5 */

    /* The root namespace, inherited by every task until one clones it. */
    vfs_bind("/",      FS_TASK_ID);
    vfs_bind("/dev/",  CONSOLE_TASK_ID);
    vfs_bind("/proc/", PROC_TASK_ID);

    kprintf("[boot] 4 servers, 2 apps; root namespace: / /dev/ /proc/\n");
    scheduler_start();
}
