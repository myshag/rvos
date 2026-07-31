/* sh.c — an interactive shell. It reads a line from the console server,
   splits it, and hands it to the loader, which is a function in the shared
   user text: same code, but the scratch buffer is ours because writable
   state is not shared between programs.

   Nothing here is privileged. Reading the keyboard is a message to the
   console server; running a program is three syscalls made on our behalf by
   spawn(). */
#include "syscall.h"
#include "vfs.h"
#include "ulib.h"

#define LINEMAX 128
#define ELFMAX  16384

static char line[LINEMAX];
static char elfbuf[ELFMAX];
static char *argv[8];

int spawn(const char *path, char *scratch, int scratchsz,
          int argc, char *const argv[]);   /* loader.c */

/* One character. This used to spin — read, get 0, yield, read again — because
   the console server always answered at once. It does not any more: a read
   with nothing to read is kept until a key arrives, and this call simply does
   not return until then. The loop is left in place for the case where a
   second reader is told 0, and it no longer runs. */
static int getc_blocking(int fd)
{
    for (;;) {
        char c;
        int n = vfs_read(fd, &c, 1);
        if (n > 0)
            return (unsigned char)c;
        _ecall1(SYS_YIELD, 0);
    }
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Print a file. The point of "everything is a file" is lost if the shell
   cannot look at one, and the things worth looking at are not files at all:
   /proc/tasks is rendered by a server, /net/status by the protocol stack.
   This does not know or care which. */
static void cat(const char *path)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        uputs("sh: cannot open ");
        uputs(path);
        uputs("\n");
        return;
    }
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

static int split(char *s, char **out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ')
            *s++ = 0;
        if (!*s)
            break;
        out[n++] = s;
        while (*s && *s != ' ')
            s++;
    }
    return n;
}

void sh_main(void)
{
    unsigned long go;
    sys_recv(&go, (int)sizeof(go));

    int con = vfs_open("/dev/console");
    if (con < 0) {
        uputs("sh: no console\n");
        sys_exit();
    }

    uputs("\n--- sh (U-mode) ----------------------------------------\n");
    uputs("type a path to run it, e.g. /HELLO.ELF one two\n");

    for (;;) {
        uputs("\nrvos$ ");
        int len = 0;
        for (;;) {
            int c = getc_blocking(con);
            if (c == '\r' || c == '\n') {
                uputs("\n");
                break;
            }
            if ((c == 8 || c == 127) && len > 0) {   /* backspace */
                len--;
                uputs("\b \b");
                continue;
            }
            if (c >= ' ' && len < LINEMAX - 1) {
                line[len++] = (char)c;
                char e[2] = { (char)c, 0 };
                uputs(e);
            }
        }
        line[len] = 0;
        if (len == 0)
            continue;

        int argc = split(line, argv, 8);
        if (argc == 0)
            continue;

        if (streq(argv[0], "cat")) {
            if (argc < 2)
                uputs("usage: cat <path>\n");
            else
                for (int i = 1; i < argc; i++)
                    cat(argv[i]);
            continue;
        }

        if (streq(argv[0], "create")) {
            if (argc < 2) {
                uputs("usage: create <path> [text...]\n");
                continue;
            }
            char msg[LINEMAX];
            int k = 0;
            for (int i = 2; i < argc; i++) {
                if (i > 2)
                    msg[k++] = ' ';
                for (const char *q = argv[i]; *q && k < LINEMAX - 2; q++)
                    msg[k++] = *q;
            }
            if (k)
                msg[k++] = '\n';
            int fd = vfs_create(argv[1]);
            if (fd < 0) {
                uputs("create: refused\n");
                continue;
            }
            if (k && vfs_write(fd, msg, k) < 0)
                uputs("create: write refused\n");
            vfs_close(fd);
            continue;
        }

        if (streq(argv[0], "rm")) {
            if (argc < 2) {
                uputs("usage: rm <path>\n");
                continue;
            }
            int fd = vfs_open(argv[1]);
            if (fd < 0) {
                uputs("rm: no such file\n");
                continue;
            }
            if (vfs_ioctl(fd, IOCTL_REMOVE) < 0)
                uputs("rm: refused\n");
            vfs_close(fd);
            continue;
        }

        /* Text to a file, which is how a control file is spoken to. Not a
           redirection — there are no pipes here — just the write a program
           would do, available from the prompt. */
        if (streq(argv[0], "write")) {
            if (argc < 3) {
                uputs("usage: write <path> <text...>\n");
                continue;
            }
            char msg[LINEMAX];
            int k = 0;
            for (int i = 2; i < argc; i++) {
                if (i > 2)
                    msg[k++] = ' ';
                for (const char *q = argv[i]; *q && k < LINEMAX - 1; q++)
                    msg[k++] = *q;
            }
            msg[k] = 0;
            int fd = vfs_open(argv[1]);
            if (fd < 0) {
                uputs("write: cannot open ");
                uputs(argv[1]);
                uputs("\n");
                continue;
            }
            if (vfs_write(fd, msg, k) < 0)
                uputs("write: refused\n");
            char back[VFS_DATA_MAX + 1];
            int n = vfs_read(fd, back, VFS_DATA_MAX);
            if (n > 0) {
                back[n] = 0;
                uputs(back);
            }
            vfs_close(fd);
            continue;
        }

        /* Start a proxy for another machine's namespace and mount it, which
           is two calls and no new mechanism. */
        if (streq(argv[0], "import")) {
            if (argc < 4) {
                uputs("usage: import <a.b.c.d> <port> <prefix>\n");
                continue;
            }
            char *av[4];
            av[0] = (char *)"/IMPORT.ELF";
            av[1] = argv[1];
            av[2] = argv[2];
            av[3] = argv[3];
            int tid = spawn(av[0], elfbuf, ELFMAX, 4, av);
            if (tid < 0)
                uputs("import: cannot run /IMPORT.ELF\n");
            else if (sys_mount(argv[3], tid, MREPL) < 0)
                uputs("import: no room in the mount table\n");
            else
                uputs("mounted\n");
            continue;
        }

        /* The other builtin, because it answers the question a loader raises:
           does running programs cost memory permanently? */
        if (streq(argv[0], "mem")) {
            int mem[2] = { 0, 0 };
            char n[24];
            sys_meminfo(mem);
            int k = uutoa((unsigned long)mem[0], n);
            n[k] = 0;
            uputs("free pages: ");
            uputs(n);
            uputs("\n");
            continue;
        }

        int tid = spawn(argv[0], elfbuf, ELFMAX, argc, argv);
        if (tid < 0) {
            uputs("sh: cannot run ");
            uputs(argv[0]);
            uputs("\n");
        }
    }
}
