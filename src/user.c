/* user.c — the first program that is not part of the kernel.

   Everything in this file is linked into its own page-aligned region (see
   kernel.ld) and mapped with the U bit, so it runs in user mode. That single
   bit changes what the code is allowed to be: it cannot call kprintf, cannot
   read the task table, cannot touch the mount table — every kernel page in
   its address space lacks the U bit, and U-mode access to such a page is
   refused by the hardware. What it *can* do is trap into the kernel.

   So this file uses nothing but syscalls, and reaches the filesystem and the
   console the same way any task does: by sending messages to the servers.
   The Plan 9 interface survives the privilege drop unchanged — vfs.h is a
   header of syscall wrappers, which is exactly why it still works here. */
#include "syscall.h"
#include "vfs.h"

static int ulen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

/* Kernel-mediated console: one character per trap. Crude, but it belongs to
   nobody else and needs no server. */
static void uputs(const char *s)
{
    while (*s)
        _ecall1(SYS_PUTC, *s++);
}

/* Servers share their text but not their state. This reaches for the
   filesystem server's private data region — its open-file cache — which is
   mapped into that server's address space and no other. Linker symbols are
   just addresses; resolving one costs nothing at run time and gets us
   nowhere. */
void peeker_main(void)
{
    extern char __ufs_start[];
    unsigned long go;
    sys_recv(&go, (int)sizeof(go));

    uputs("\n--- peeker (U-mode) ------------------------------------\n");
    uputs("$ read the fs server's private data region\n");
    volatile char *other = (volatile char *)__ufs_start;
    char c = *other;

    uputs("  UNREACHABLE: read succeeded, servers are not isolated\n");
    (void)c;
    for (;;)
        _ecall1(SYS_YIELD, 0);
}

void user_main(void)
{
    /* Wait to be told to start, so the demo output stays in order. */
    unsigned long go;
    sys_recv(&go, (int)sizeof(go));

    uputs("\n--- user program (U-mode) ------------------------------\n");
    uputs("$ running with the U bit; kernel pages are unreachable\n");

    /* The whole interface, from user mode, over nothing but ecall. */
    uputs("\n$ write(/dev/console)   -- namespace + IPC, all via syscalls\n");
    int fd = vfs_open("/dev/console");
    if (fd < 0) {
        uputs("  open failed\n");
    } else {
        const char *msg = "  printed by the console server on our behalf\n";
        vfs_write(fd, msg, ulen(msg));
        vfs_close(fd);
    }

    uputs("\n$ cat /proc/tasks\n");
    fd = vfs_open("/proc/tasks");
    if (fd >= 0) {
        for (;;) {
            char buf[VFS_DATA_MAX + 1];
            int n = vfs_read(fd, buf, VFS_DATA_MAX);
            if (n <= 0)
                break;
            buf[n] = 0;
            uputs(buf);
        }
        vfs_close(fd);
    }

    /* Now reach for the kernel itself. The page is mapped in this address
       space — the trap vector needs it — but without the U bit, so the MMU
       refuses a user-mode load. This is the boundary the previous stage
       could not draw: isolation from the kernel, not just from peers. */
    uputs("\n$ read *(char*)0x80000000   -- kernel text, mapped but U-less\n");
    volatile char *kernel = (volatile char *)0x80000000UL;
    char c = *kernel;

    uputs("  UNREACHABLE: read succeeded, isolation failed\n");
    (void)c;
    for (;;)
        _ecall1(SYS_YIELD, 0);
}
