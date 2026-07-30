/* hello.c — a program in the ordinary sense: its own ELF file, sitting on the
   FAT16 volume, loaded into a fresh address space at run time. Nothing about
   it is known at kernel build time; it is not linked into kernel.elf and its
   entry point is discovered by reading its program headers.

   It is self-contained because the headers it uses are self-contained:
   syscall.h is inline assembly and vfs.h is a client library built on it, so
   there is nothing to link against. */
#include "syscall.h"
#include "vfs.h"

static int hlen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void hputs(const char *s)
{
    while (*s)
        _ecall1(SYS_PUTC, *s++);
}

/* Placed first by the link script; the loader jumps here via e_entry. */
__attribute__((section(".text.start"))) void _start(void)
{
    hputs("\n  [hello] loaded from /HELLO.ELF into a fresh address space\n");
    hputs("  [hello] nothing here was linked into the kernel\n");

    /* And it is a first-class citizen: the namespace and the servers work
       for it exactly as for anything else. */
    int fd = vfs_open("/dev/console");
    if (fd >= 0) {
        const char *m = "  [hello] talking to the console server over IPC\n";
        vfs_write(fd, m, hlen(m));
        vfs_close(fd);
    }

    fd = vfs_open("/README.TXT");
    if (fd >= 0) {
        char buf[VFS_DATA_MAX + 1];
        int n = vfs_read(fd, buf, 64);
        if (n > 0) {
            buf[n] = 0;
            hputs("  [hello] first bytes of /README.TXT via the fs server:\n    ");
            hputs(buf);
        }
        vfs_close(fd);
    }

    hputs("  [hello] done\n");
    for (;;)
        _ecall1(SYS_YIELD, 0);
}
