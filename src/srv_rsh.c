/* srv_rsh.c — the shell, reached over TCP, and by telnet.

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

/* The shell's own text, with the line endings a terminal on the far end
   expects. `wr` stays raw: telnet's own bytes must not be rewritten. */
static int puts_conn(const char *s)
{
    char buf[128];
    int k = 0;
    for (; *s; s++) {
        if (k > (int)sizeof(buf) - 2) {
            if (wr(buf, k) < 0)
                return -1;
            k = 0;
        }
        if (*s == '\n')
            buf[k++] = '\r';
        buf[k++] = *s;
    }
    return k ? wr(buf, k) : 0;
}

static void put_num(unsigned long v)
{
    int n = uutoa(v, scratch);
    scratch[n] = 0;
    puts_conn(scratch);
}

/* ---- telnet ------------------------------------------------------------
   `nc` sends the bytes you type and nothing else. `telnet` sends the bytes
   you type *and* a conversation about how to send them, mixed into the same
   stream and marked by IAC — 255, "interpret as command". Without this the
   negotiation lands in the line buffer and the shell answers

       rsh: no such command: \xff\xfd\x03\xff\xfb\x18ls

   which is exactly what it did.

   Two rules make the rest small. A literal 255 in the data is sent as two of
   them, so IAC IAC is one byte of data. And a negotiation is answered only
   when it is an *offer* — WILL and DO — because answering a refusal with a
   refusal is how two implementations negotiate for ever.

   Everything is refused, which leaves the connection in the default mode of
   RFC 854: the client keeps local echo and sends whole lines. That is the
   same arrangement `nc` gives us by having a terminal do it, so one code path
   serves both. */

#define IAC  255
#define SEo  240
#define SBo  250
#define WILL 251
#define WONT 252
#define DO   253
#define DONT 254

enum { T_DATA, T_IAC, T_OPT, T_SB, T_SB_IAC };
static int tstate;
static unsigned tcmd;

static void telnet_refuse(unsigned cmd, unsigned opt)
{
    char r[3];
    r[0] = (char)IAC;
    r[1] = (char)(cmd == WILL ? DONT : WONT);
    r[2] = (char)opt;
    wr(r, 3);
}

/* Raw bytes in, data bytes into inbuf, answers straight back out. */
static void telnet_in(const char *raw, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned c = (unsigned char)raw[i];
        switch (tstate) {
        case T_DATA:
            if (c == IAC) {
                tstate = T_IAC;
            } else if (inlen < INBUF) {
                inbuf[inlen++] = (char)c;
            }
            break;
        case T_IAC:
            if (c == IAC) {                 /* two of them mean one of them */
                if (inlen < INBUF)
                    inbuf[inlen++] = (char)c;
                tstate = T_DATA;
            } else if (c == WILL || c == WONT || c == DO || c == DONT) {
                tcmd = c;
                tstate = T_OPT;
            } else if (c == SBo) {
                tstate = T_SB;
            } else {
                tstate = T_DATA;            /* NOP, AYT, break: nothing to do */
            }
            break;
        case T_OPT:
            if (tcmd == WILL || tcmd == DO)
                telnet_refuse(tcmd, c);     /* a WONT needs no answer */
            tstate = T_DATA;
            break;
        case T_SB:                          /* a subnegotiation, to be skipped */
            if (c == IAC)
                tstate = T_SB_IAC;
            break;
        case T_SB_IAC:
            tstate = (c == SEo) ? T_DATA : T_SB;
            break;
        }
    }
}

/* One line from the connection, without the ending. Returns -1 at end of
   file. TCP is a stream, so a line may arrive in pieces or two may arrive at
   once; the leftovers stay in inbuf for the next call.

   Line endings are whatever the client believes in: nc sends LF, telnet sends
   CR LF, and a telnet sending a bare carriage return sends CR NUL. Leading
   remnants of any of them are dropped rather than reported as empty lines. */
static int readline(char *out, int cap)
{
    for (;;) {
        int skip = 0;
        while (skip < inlen && (inbuf[skip] == '\n' || inbuf[skip] == '\r' ||
                                inbuf[skip] == 0))
            skip++;
        if (skip) {
            for (int k = skip; k < inlen; k++)
                inbuf[k - skip] = inbuf[k];
            inlen -= skip;
        }

        for (int i = 0; i < inlen; i++) {
            if (inbuf[i] != '\n' && inbuf[i] != '\r')
                continue;
            int n = i;
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
        char raw[VFS_DATA_MAX];
        int n = vfs_read(conn, raw, (int)sizeof(raw));
        if (n <= 0)
            return -1;                  /* 0 = the far end closed its half */
        telnet_in(raw, n);
    }
}

/* Words, with quotes — which stopped being optional the moment a file could
   be called "a rather long file name.txt". Only double quotes, and no escape
   inside them: enough to name a file, and not the beginning of a language. */
static int split(char *s, char **out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ')
            *s++ = 0;
        if (!*s)
            break;
        if (*s == '"') {
            *s++ = 0;
            out[n++] = s;
            while (*s && *s != '"')
                s++;
            if (*s)
                *s++ = 0;
        } else {
            out[n++] = s;
            while (*s && *s != ' ')
                s++;
        }
    }
    return n;
}

/* ---- builtins ---------------------------------------------------------
   Almost none. What is left changes *this shell's own namespace*, which is
   the one thing a separate program cannot usefully do on its behalf, and
   `echo`, whose output has to come back here rather than wherever
   /dev/console happens to point. Everything else is a file in /BIN. */

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
              "  create <path> [text...]      make a file and put text in it\n"
              "  mkdir <path>       make a directory\n"
              "  rm <path>          delete a file or an empty directory\n"
              "  import <ip> <port> <prefix>  mount a namespace from another\n"
              "                     machine at <prefix>\n"
              "  net            interface, ARP, connections (/net/status)\n"
              "  mem            free and total pages\n"
              "  run <path> ..  start a program; its output comes back here\n"
              "  echo <words>   write them back\n"
              "  exit           hang up\n");
}

/* Where a bare word is looked for: /BIN, unless it already has a slash in it.
   Nothing here uppercases anything — the filesystem does, since an 8.3 name
   is stored and looked up upper-cased, so `ls` finds /BIN/LS.ELF without this
   code knowing that FAT16 shouts. */
static void find_program(const char *word, char *out, int cap)
{
    int has_slash = 0;
    for (const char *p = word; *p; p++)
        if (*p == '/')
            has_slash = 1;

    int n = 0;
    if (!has_slash)
        for (const char *pre = "/BIN/"; *pre && n < cap - 6; pre++)
            out[n++] = *pre;
    for (const char *p = word; *p && n < cap - 5; p++)
        out[n++] = *p;
    if (!has_slash)
        for (const char *ext = ".ELF"; *ext && n < cap - 1; ext++)
            out[n++] = *ext;
    out[n] = 0;
}

/* Start it and wait, unless the line ended in `&`.

   Waiting is what makes a shell of external commands usable: the child writes
   this same connection, so without it the prompt and the program's first line
   arrive interleaved. That used to be an accepted wart because `cat` was a
   builtin. It is not one now. */
static void run_command(int argc, char **argv)
{
    int background = 0;
    if (argc > 1 && streq(argv[argc - 1], "&")) {
        background = 1;
        argc--;
    }
    char path[VFS_PATH_MAX];
    find_program(argv[0], path, (int)sizeof(path));

    int tid = spawn(path, elfbuf, ELFMAX, argc, argv);
    if (tid < 0) {
        puts_conn("rsh: no such command: ");
        puts_conn(argv[0]);
        puts_conn("  (try `help`)\n");
        return;
    }
    if (background) {
        puts_conn("[");
        put_num((unsigned long)tid);
        puts_conn("]\n");
    } else {
        sys_wait(tid);
    }
}

/* ---- one session ------------------------------------------------------ */

static void session(int slot)
{
    inlen  = 0;
    tstate = T_DATA;
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
        } else if (streq(argv[0], "echo")) {
            /* The one command kept, because its output has to arrive *here*
               rather than wherever /dev/console points — which for a program
               started from this shell is here anyway, but for `echo` there is
               nothing to start. */
            for (int i = 1; i < argc; i++) {
                if (i > 1)
                    puts_conn(" ");
                puts_conn(argv[i]);
            }
            puts_conn("\n");
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
        } else if (streq(argv[0], "unmount")) {
            if (argc < 2)
                puts_conn("usage: unmount <name>\n");
            else if (sys_unmount(argv[1]) < 0)
                puts_conn("unmount: nothing bound there\n");
        } else if (streq(argv[0], "import")) {
            /* Two steps and nothing else: start the proxy, and mount it. It
               is a task, and a mount takes a task — the namespace has no
               notion of "remote" and does not need one. */
            if (argc < 4) {
                puts_conn("usage: import <a.b.c.d> <port> <prefix>\n");
            } else {
                char *av[4];
                av[0] = (char *)"/BIN/IMPORT.ELF";
                av[1] = argv[1];
                av[2] = argv[2];
                av[3] = argv[3];        /* its own mount point, to strip */
                int tid = spawn(av[0], elfbuf, ELFMAX, 4, av);
                if (tid < 0) {
                    puts_conn("import: cannot run /BIN/IMPORT.ELF\n");
                } else if (sys_mount(argv[3], tid, MREPL) < 0) {
                    puts_conn("import: no room in the mount table\n");
                } else {
                    puts_conn("mounted, task ");
                    put_num((unsigned long)tid);
                    puts_conn("\n");
                }
            }
        } else {
            run_command(argc, argv);
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
