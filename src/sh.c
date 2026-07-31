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
#define ELFMAX  8192

static char line[LINEMAX];
static char elfbuf[ELFMAX];
static char *argv[8];

int spawn(const char *path, char *scratch, int scratchsz,
          int argc, char *const argv[]);   /* loader.c */

/* Blocking read of one character: the console server answers immediately
   with 0 when nothing has been typed, so we yield and ask again. */
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
