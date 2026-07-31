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

   A program started from here has its output arrive on the connection, and
   the whole of the mechanism is one line: bind("/net/tcp/N", "/dev/console")
   in this shell's namespace. A task it creates inherits that namespace; the
   program writes to /dev/console because that is where programs write; the
   network server is asked for /net/tcp/N, a name it has always understood.
   Nobody in that chain knows about anybody else. */
#include "syscall.h"
#include "vfs.h"
#include "ulib.h"
#include "servers.h"

#define LINEMAX 128
#define ELFMAX  16384
#define INBUF   256

static char line[LINEMAX];
static char elfbuf[ELFMAX];
static char *argv[8];
static char inbuf[INBUF];
static int  inlen;
static char scratch[64];

int spawn(const char *path, char *sc, int scsz,
          int argc, char *const argv[]);   /* loader.c */

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

/* A directory that is a union is *every* member of it, one after another —
   which is the whole point of joining two names, and the one place a program
   has to know the union exists. Opening a file takes the first answer;
   listing a directory takes all of them. */
static void do_ls(const char *path)
{
    char real[VFS_PATH_MAX];
    int shown = 0;
    for (int nth = 0; ; nth++) {
        int srv = sys_resolve(path, real, (int)sizeof(real), nth);
        if (srv < 0)
            break;
        int fd = vfs_open_at(srv, real);
        if (fd < 0)
            continue;                   /* this member does not have it */
        for (;;) {
            char buf[VFS_DATA_MAX];
            int n = vfs_read(fd, buf, VFS_DATA_MAX);
            if (n <= 0)
                break;
            if (wr(buf, n) < 0)
                break;
        }
        vfs_close(fd);
        shown++;
    }
    if (!shown) {
        puts_conn("rsh: cannot open ");
        puts_conn(path);
        puts_conn("\n");
    }
}

static void do_help(void)
{
    puts_conn("commands:\n"
              "  ls [path]      list a directory (default /)\n"
              "  cat <path>...  print a file, a report, or a connection\n"
              "  ps             running tasks              (/proc/tasks)\n"
              "  mounts         this shell's namespace     (/proc/mounts)\n"
              "  bind [-a|-b] <old> <new>   make <new> mean <old>; -a and -b\n"
              "                     join what is there instead of replacing it\n"
              "  mount [-a|-b] <pfx> <task> put a server behind a name\n"
              "  unmount <name>     take a name back\n"
              "  import <ip> <port> <prefix>  mount a namespace from another\n"
              "                     machine at <prefix>\n"
              "  net            interface, ARP, connections (/net/status)\n"
              "  mem            free and total pages\n"
              "  run <path> ..  start a program; its output comes back here\n"
              "  echo <words>   write them back\n"
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
            do_ls(argc > 1 ? argv[1] : "/");
        } else if (streq(argv[0], "ps")) {
            do_cat("/proc/tasks");
        } else if (streq(argv[0], "mounts")) {
            /* Whose namespace? This shell's — and the answer would differ for
               a task that had cloned and rebound its own. That is why the
               proc server has to be told which task to report on rather than
               having "the" mount table to look at. */
            do_cat("/proc/mounts");
        } else if (streq(argv[0], "bind") || streq(argv[0], "mount")) {
            /* -a and -b are Plan 9's spelling of "join what is already there,
               after it" and "…before it". Without one, replace. */
            int f = MREPL, a = 1;
            if (argc > 1 && argv[1][0] == '-' && argv[1][2] == 0) {
                if (argv[1][1] == 'a') { f = MAFTER;  a = 2; }
                if (argv[1][1] == 'b') { f = MBEFORE; a = 2; }
            }
            int mnt = streq(argv[0], "mount");
            if (argc < a + 2) {
                puts_conn(mnt ? "usage: mount [-a|-b] <prefix> <task>\n"
                              : "usage: bind [-a|-b] <old> <new>\n");
            } else if (mnt) {
                int t = 0;
                const char *d = argv[a + 1];
                while (*d >= '0' && *d <= '9')
                    t = t * 10 + (*d++ - '0');
                if (sys_mount(argv[a], t, f) < 0)
                    puts_conn("mount: no room in the mount table\n");
            } else if (sys_bind(argv[a], argv[a + 1], f) < 0) {
                puts_conn("bind: no room in the mount table\n");
            }
        } else if (streq(argv[0], "import")) {
            /* Two steps and nothing else: start the proxy, and mount it. It
               is a task, and a mount takes a task — the namespace has no
               notion of "remote" and does not need one. */
            if (argc < 4) {
                puts_conn("usage: import <a.b.c.d> <port> <prefix>\n");
            } else {
                char *av[4];
                av[0] = (char *)"/IMPORT.ELF";
                av[1] = argv[1];
                av[2] = argv[2];
                av[3] = argv[3];        /* its own mount point, to strip */
                int tid = spawn(av[0], elfbuf, ELFMAX, 4, av);
                if (tid < 0) {
                    puts_conn("import: cannot run /IMPORT.ELF\n");
                } else if (sys_mount(argv[3], tid, MREPL) < 0) {
                    puts_conn("import: no room in the mount table\n");
                } else {
                    puts_conn("mounted, task ");
                    put_num((unsigned long)tid);
                    puts_conn("\n");
                }
            }
        } else if (streq(argv[0], "unmount")) {
            if (argc < 2)
                puts_conn("usage: unmount <name>\n");
            else if (sys_unmount(argv[1]) < 0)
                puts_conn("unmount: nothing bound there\n");
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
                /* Announced *before* it is started, not after. The child
                   and this shell write the same connection, and the child is
                   running the instant spawn returns — a line printed
                   afterwards arrives in the middle of the program's first
                   one. There is no wait() here and no job control, so the
                   prompt below still comes back while the program is talking;
                   `ps` says what is running. */
                puts_conn("starting ");
                puts_conn(argv[1]);
                puts_conn("\n");
                if (spawn(argv[1], elfbuf, ELFMAX,
                          argc - 1, argv + 1) < 0) {
                    puts_conn("rsh: cannot run ");
                    puts_conn(argv[1]);
                    puts_conn("\n");
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

    /* Our own view of the tree, so that what we bend does not bend anybody
       else's — and one binding in it: /dev/console now means the network.
       Every task we create inherits this, which is the whole mechanism by
       which a program's output follows it down a connection. */
    /* Our own view of the tree, so that what we bend does not bend anybody
       else's. The binding itself is per session, below: which connection
       /dev/console means depends on who is logged in. (sys_nsclone, not
       vfs_ns_clone — that one is the kernel's own copy of this function, and
       this task may not call into kernel text at all.) */
    sys_nsclone();

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

        /* One line, and it is the whole of output redirection: in this
           shell's namespace — and so in the namespace of every program it
           starts — /dev/console now means this connection. The programs know
           nothing about it, and neither does the network server: it is asked
           for /net/tcp/N, which is a name it has always understood. */
        sys_bind(path, "/dev/console", MREPL);

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
