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
__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    hputs("\n  [hello] loaded from a file into a fresh address space\n");

    /* argc/argv arrive in a0/a1, exactly where the calling convention puts
       the first two parameters — the loader planted the block in our stack
       before we ever ran. */
    hputs("  [hello] argv:");
    for (int i = 0; i < argc; i++) {
        hputs(" ");
        hputs(argv[i]);
    }
    hputs("\n");

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

    /* The network, reached exactly like a file — this program contains no
       notion of ethernet, virtqueues or TCP. */
    fd = vfs_open("/net/status");
    if (fd >= 0) {
        char buf[VFS_DATA_MAX + 1];
        int n = vfs_read(fd, buf, VFS_DATA_MAX);
        if (n > 0) {
            buf[n] = 0;
            hputs("  [hello] cat /net/status:\n");
            hputs(buf);
        }
        vfs_close(fd);
    }

    /* The connection may still be coming up — a segment was deliberately
       lost and the stack is waiting on its retransmit timer. A program that
       wants the network waits for it, exactly as it would anywhere else. */
    fd = vfs_open("/net/tcp");
    if (fd >= 0) {
        const char *m = "hello from a loaded program\n";
        int ok = 0;
        for (int try = 0; try < 4000 && !ok; try++) {
            if (vfs_write(fd, m, hlen(m)) > 0)
                ok = 1;
            else
                _ecall1(SYS_YIELD, 0);
        }
        hputs(ok ? "  [hello] wrote to /net/tcp\n"
                 : "  [hello] /net/tcp never came up\n");

        /* Two more, back to back and without waiting for either to be
           acknowledged. Until the stack had a send buffer the second of these
           moved nothing and returned 0, because one segment could be in
           flight at a time; now all three are the stack's problem and not
           this program's. */
        if (ok) {
            vfs_write(fd, "second line\n", 12);
            vfs_write(fd, "third line\n", 11);
            hputs("  [hello] two more writes, neither waiting on the first\n");
        }

        if (ok) {
            char buf[VFS_DATA_MAX + 1];
            for (int try = 0; try < 4000; try++) {
                int n = vfs_read(fd, buf, VFS_DATA_MAX);
                if (n > 0) {
                    buf[n] = 0;
                    hputs("  [hello] read back from /net/tcp: ");
                    hputs(buf);
                    break;
                }
                _ecall1(SYS_YIELD, 0);
            }
        }
        vfs_close(fd);
    }

    hputs("  [hello] done\n");
    sys_exit();                 /* hand our pages back */
}
