/* srv_rsh.c — the shell, reached over TCP.

   `sh.c` reads from /dev/console and writes to it. This reads from a TCP
   connection and writes to it, and is otherwise the same idea: a line, split
   into words, either a builtin or a path to run. Nothing about the network is
   visible here beyond four lines of setup — it asks /net/ctl for a port, and
   from then on the connection is a file like any other.

   It is a task in this image rather than a program on the disk for one
   reason: spawn() lives in the shared user text that kernel.elf links, and a
   program loaded from FAT16 cannot reach it. Everything else it does, a disk
   program could.

   One honest limit, and it is visible the moment you use it: a program this
   shell starts writes with SYS_PUTC, which goes to the serial console and not
   down the connection. Redirecting it means giving a task a notion of where
   its output goes, and that is a change to how every program writes, not a
   change here. Until then `run` reports the task id and says where to look. */
#include "syscall.h"
#include "vfs.h"
#include "ulib.h"
#include "servers.h"

#define LINEMAX 128
#define ELFMAX  8192
#define INBUF   256

static char line[LINEMAX];
static char elfbuf[ELFMAX];
static char *argv[8];
static char inbuf[INBUF];
static int  inlen;
static char scratch[64];

int spawn(const char *path, char *sc, int scsz,
          int argc, char *const argv[]);       /* loader.c */

static int rlen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---- talking to the connection ----------------------------------------
   A write goes into the stack's send buffer and returns how much fitted; a
   full buffer returns 0. There is no blocking write — a read parks, a write
   does not — so this is the one place in the program that spins, and it is
   marked as such rather than hidden. */
static int conn = -1;

static int wr(const char *s, int n)
{
    int off = 0;
    while (off < n) {
        int k = vfs_write(conn, s + off, n - off);
        if (k < 0)
            return -1;                  /* the connection is gone */
        if (k == 0) {
            sys_yield();                /* the send buffer is full: wait */
            continue;
        }
        off += k;
    }
    return off;
}

static int puts_conn(const char *s) { return wr(s, rlen(s)); }

static void put_num(unsigned long v)
{
    int n = uutoa(v, scratch);
    scratch[n] = 0;
    puts_conn(scratch);
}

/* One line from the connection, without the newline. Returns -1 at end of
   file. TCP is a stream, so a line may arrive in pieces or two may arrive at
   once; the leftovers stay in inbuf for the next call. */
static int readline(char *out, int cap)
{
    for (;;) {
        for (int i = 0; i < inlen; i++) {
            if (inbuf[i] != '\n')
                continue;
            int n = i;
            if (n > 0 && inbuf[n - 1] == '\r')
                n--;                    /* telnet and nc send CR LF */
            if (n > cap - 1)
                n = cap - 1;
            umemcpy(out, inbuf, (unsigned long)n);
            out[n] = 0;
            int rest = inlen - (i + 1);
            for (int k = 0; k < rest; k++)
                inbuf[k] = inbuf[i + 1 + k];
            inlen = rest;
            return n;
        }
        if (inlen >= INBUF) {           /* a line longer than we will take */
            inlen = 0;
            out[0] = 0;
            return 0;
        }
        int n = vfs_read(conn, inbuf + inlen, INBUF - inlen);
        if (n <= 0)
            return -1;                  /* 0 = the far end closed its half */
        inlen += n;
    }
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

/* ---- builtins ---------------------------------------------------------
   `cat` is the interesting one, and it is interesting because it is dull:
   the path may be a file on the FAT16 volume, a report rendered by the proc
   server, or the state of a TCP connection, and this code cannot tell which.
   That is the whole point of the interface these stages have been building —
   read over a network what a server renders on request, with no part of the
   chain knowing about any other. */
static void do_cat(const char *path)
{
    int fd = vfs_open(path);
    if (fd < 0) {
        puts_conn("rsh: cannot open ");
        puts_conn(path);
        puts_conn("\n");
        return;
    }
    for (;;) {
        char buf[VFS_DATA_MAX];
        int n = vfs_read(fd, buf, VFS_DATA_MAX);
        if (n <= 0)
            break;
        if (wr(buf, n) < 0)
            break;
    }
    vfs_close(fd);
}

static void do_help(void)
{
    puts_conn("commands:\n"
              "  cat <path>     print a file, a report, or a connection\n"
              "  ls             what is on the disk\n"
              "  ps             the task table\n"
              "  mem            free pages\n"
              "  net            the interface, the ARP cache, the connections\n"
              "  run <path> ..  start a program (its output goes to the serial\n"
              "                 console, not here — see the source for why)\n"
              "  echo <words>\n"
              "  exit           hang up\n");
}

/* ---- one session ------------------------------------------------------ */

static void session(int slot)
{
    inlen = 0;
    puts_conn("\nrvos — you are on the guest, over its own TCP stack.\n"
              "type `help`. connection ");
    put_num((unsigned long)slot);
    puts_conn("\n");

    for (;;) {
        if (puts_conn("\nrvos# ") < 0)
            return;
        if (readline(line, LINEMAX) < 0)
            return;                     /* the caller hung up */

        int argc = split(line, argv, 8);
        if (argc == 0)
            continue;

        if (streq(argv[0], "exit") || streq(argv[0], "quit")) {
            puts_conn("goodbye\n");
            return;
        }
        if (streq(argv[0], "help")) {
            do_help();
        } else if (streq(argv[0], "cat")) {
            if (argc < 2)
                puts_conn("usage: cat <path>\n");
            else
                for (int i = 1; i < argc; i++)
                    do_cat(argv[i]);
        } else if (streq(argv[0], "ls")) {
            do_cat("/");
        } else if (streq(argv[0], "ps")) {
            do_cat("/proc/tasks");
        } else if (streq(argv[0], "net")) {
            do_cat("/net/status");
        } else if (streq(argv[0], "echo")) {
            for (int i = 1; i < argc; i++) {
                if (i > 1)
                    puts_conn(" ");
                puts_conn(argv[i]);
            }
            puts_conn("\n");
        } else if (streq(argv[0], "mem")) {
            int info[2] = { 0, 0 };
            sys_meminfo(info);
            puts_conn("free pages: ");
            put_num((unsigned long)info[0]);
            puts_conn(" of ");
            put_num((unsigned long)info[1]);
            puts_conn("\n");
        } else if (streq(argv[0], "run")) {
            if (argc < 2) {
                puts_conn("usage: run <path> [args]\n");
            } else {
                int tid = spawn(argv[1], elfbuf, ELFMAX, argc - 1, argv + 1);
                if (tid < 0) {
                    puts_conn("rsh: cannot run ");
                    puts_conn(argv[1]);
                    puts_conn("\n");
                } else {
                    puts_conn("started as task ");
                    put_num((unsigned long)tid);
                    puts_conn("; its output goes to the serial console\n");
                }
            }
        } else {
            puts_conn("rsh: no such command: ");
            puts_conn(argv[0]);
            puts_conn("  (try `help`)\n");
        }
    }
}

/* ---- the port ---------------------------------------------------------- */

/* Write one command to /net/ctl and read the answer. Identical in shape to
   what netd does, because it is the same interface. */
static int ctl(const char *cmd, char *answer, int cap)
{
    int fd = vfs_open("/net/ctl");
    if (fd < 0)
        return -1;
    if (vfs_write(fd, cmd, rlen(cmd)) < 0) {
        vfs_close(fd);
        return -1;
    }
    int n = vfs_read(fd, answer, cap - 1);
    vfs_close(fd);
    if (n <= 0)
        return -1;
    answer[n] = 0;
    return n;
}

static int slot_of(const char *answer)
{
    if (answer[0] != 'o' || answer[1] != 'k')
        return -1;
    const char *s = answer + 2;
    while (*s == ' ')
        s++;
    if (*s < '0' || *s > '9')
        return -1;
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

#define RSH_PORT 23

void rsh_main(void)
{
    uint64 go;
    sys_recv(&go, (int)sizeof(go));

    char answer[64];
    if (ctl("listen 23", answer, sizeof(answer)) < 0 ||
        slot_of(answer) < 0) {
        uputs("  [rsh] could not take port 23\n");
        sys_exit();
    }
    int lslot = slot_of(answer);
    uputs("  [rsh] a shell is listening on port 23\n");

    for (;;) {
        char cmd[24];
        int k = 0;
        const char *a = "accept ";
        while (*a)
            cmd[k++] = *a++;
        if (lslot >= 10)
            cmd[k++] = (char)('0' + lslot / 10);
        cmd[k++] = (char)('0' + lslot % 10);
        cmd[k] = 0;

        if (ctl(cmd, answer, sizeof(answer)) < 0)
            break;
        int slot = slot_of(answer);
        if (slot < 0)
            break;

        char path[24];
        k = 0;
        a = "/net/tcp/";
        while (*a)
            path[k++] = *a++;
        if (slot >= 10)
            path[k++] = (char)('0' + slot / 10);
        path[k++] = (char)('0' + slot % 10);
        path[k] = 0;

        conn = vfs_open(path);
        if (conn < 0)
            continue;
        uputs("  [rsh] someone logged in\n");
        session(slot);
        vfs_close(conn);               /* the last descriptor: this also
                                          closes the connection */
        conn = -1;
        uputs("  [rsh] they logged out\n");
    }

    uputs("  [rsh] stopping\n");
    sys_exit();
}
