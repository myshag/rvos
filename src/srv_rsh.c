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
#include "malloc.h"

#define LINEMAX 128
#define INBUF   256

/* Всё, что раньше было файловыми статиками, — состояние одной сессии.
   Нити делят данные задачи, поэтому две сессии в одном сервере затоптали бы
   друг друга на первой же строке ввода. Структура выделяется на соединение и
   передаётся указателем; больше в этом файле общего состояния нет. */
struct session {
    int      conn;
    int      slot;
    char     line[LINEMAX];
    char    *argv[8];
    char     inbuf[INBUF];
    int      inlen;
    int      tstate;                /* разбор telnet — тоже своё у каждого */
    unsigned tcmd;
    char     scratch[64];
    char    *stack;                 /* её же, освобождает она сама */
};

int spawn(const char *path, int argc, char *const argv[]);   /* loader.c */

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
static int wr(struct session *S, const char *s, int n)
{
    int off = 0;
    while (off < n) {
        int k = vfs_write(S->conn, s + off, n - off);
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
static int puts_conn(struct session *S, const char *s)
{
    char buf[128];
    int k = 0;
    for (; *s; s++) {
        if (k > (int)sizeof(buf) - 2) {
            if (wr(S, buf, k) < 0)
                return -1;
            k = 0;
        }
        if (*s == '\n')
            buf[k++] = '\r';
        buf[k++] = *s;
    }
    return k ? wr(S, buf, k) : 0;
}

static void put_num(struct session *S, unsigned long v)
{
    int n = uutoa(v, S->scratch);
    S->scratch[n] = 0;
    puts_conn(S, S->scratch);
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

static void telnet_refuse(struct session *S, unsigned cmd, unsigned opt)
{
    char r[3];
    r[0] = (char)IAC;
    r[1] = (char)(cmd == WILL ? DONT : WONT);
    r[2] = (char)opt;
    wr(S, r, 3);
}

/* Raw bytes in, data bytes into inbuf, answers straight back out. */
static void telnet_in(struct session *S, const char *raw, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned c = (unsigned char)raw[i];
        switch (S->tstate) {
        case T_DATA:
            if (c == IAC) {
                S->tstate = T_IAC;
            } else if (S->inlen < INBUF) {
                S->inbuf[S->inlen++] = (char)c;
            }
            break;
        case T_IAC:
            if (c == IAC) {                 /* two of them mean one of them */
                if (S->inlen < INBUF)
                    S->inbuf[S->inlen++] = (char)c;
                S->tstate = T_DATA;
            } else if (c == WILL || c == WONT || c == DO || c == DONT) {
                S->tcmd = c;
                S->tstate = T_OPT;
            } else if (c == SBo) {
                S->tstate = T_SB;
            } else {
                S->tstate = T_DATA;            /* NOP, AYT, break: nothing to do */
            }
            break;
        case T_OPT:
            if (S->tcmd == WILL || S->tcmd == DO)
                telnet_refuse(S, S->tcmd, c);     /* a WONT needs no answer */
            S->tstate = T_DATA;
            break;
        case T_SB:                          /* a subnegotiation, to be skipped */
            if (c == IAC)
                S->tstate = T_SB_IAC;
            break;
        case T_SB_IAC:
            S->tstate = (c == SEo) ? T_DATA : T_SB;
            break;
        }
    }
}

/* One line from the connection, without the ending. Returns -1 at end of
   file. TCP is a stream, so a line may arrive in pieces or two may arrive at
   once; the leftovers stay in S->inbuf for the next call.

   Line endings are whatever the client believes in: nc sends LF, telnet sends
   CR LF, and a telnet sending a bare carriage return sends CR NUL. Leading
   remnants of any of them are dropped rather than reported as empty lines. */
static int readline(struct session *S, char *out, int cap)
{
    for (;;) {
        int skip = 0;
        while (skip < S->inlen && (S->inbuf[skip] == '\n' || S->inbuf[skip] == '\r' ||
                                S->inbuf[skip] == 0))
            skip++;
        if (skip) {
            for (int k = skip; k < S->inlen; k++)
                S->inbuf[k - skip] = S->inbuf[k];
            S->inlen -= skip;
        }

        /* Nobody is nominated while this shell is the one reading, so the
           interrupt character arrives here as a byte and means the only thing
           it can mean: forget the line. */
        for (int i = 0; i < S->inlen; i++) {
            if (S->inbuf[i] == 3) {
                S->inlen = 0;
                out[0] = 0;
                puts_conn(S, "^C\n");
                return 0;
            }
            if (S->inbuf[i] != '\n' && S->inbuf[i] != '\r')
                continue;
            int n = i;
            if (n > cap - 1)
                n = cap - 1;
            umemcpy(out, S->inbuf, (unsigned long)n);
            out[n] = 0;
            int rest = S->inlen - (i + 1);
            for (int k = 0; k < rest; k++)
                S->inbuf[k] = S->inbuf[i + 1 + k];
            S->inlen = rest;
            return n;
        }
        if (S->inlen >= INBUF) {           /* a line longer than we will take */
            S->inlen = 0;
            out[0] = 0;
            return 0;
        }
        char raw[VFS_DATA_MAX];
        int n = vfs_read(S->conn, raw, (int)sizeof(raw));
        if (n <= 0)
            return -1;                  /* 0 = the far end closed its half */
        telnet_in(S, raw, n);
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

static void do_help(struct session *S)
{
    puts_conn(S, "commands:\n"
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
static void run_command(struct session *S, int argc, char **argv)
{
    int background = 0;
    if (argc > 1 && streq(argv[argc - 1], "&")) {
        background = 1;
        argc--;
    }
    char path[VFS_PATH_MAX];
    find_program(argv[0], path, (int)sizeof(path));

    int tid = spawn(path, argc, argv);
    if (tid < 0) {
        puts_conn(S, "rsh: no such command: ");
        puts_conn(S, argv[0]);
        puts_conn(S, "  (try `help`)\n");
        return;
    }
    if (background) {
        puts_conn(S, "[");
        put_num(S, (unsigned long)tid);
        puts_conn(S, "]\n");
    } else {
        /* The connection is told who is in front of it, so that Ctrl-C has
           something to mean while this shell is not reading anything. */
        vfs_ioctl_arg(S->conn, IOCTL_INTR, tid);
        sys_wait(tid);
        if (vfs_ioctl_arg(S->conn, IOCTL_INTR, 0) > 0)
            puts_conn(S, "^C\n");
    }
}

/* ---- one session ------------------------------------------------------ */

/* Сколько стека нити-сессии. Шестнадцать килобайт — столько же, сколько у
   задачи: под сессией лежит spawn, а под ним vfs_call со своим запросом на
   664 байта в кадре. */
#define SESSION_STACK 16384

static void session(struct session *S)
{
    S->inlen  = 0;
    S->tstate = T_DATA;
    puts_conn(S, "\nrvos — you are on the guest, over its own TCP stack.\n"
              "type `help`. connection ");
    put_num(S, (unsigned long)S->slot);
    puts_conn(S, "\n");

    for (;;) {
        if (puts_conn(S, "\nrvos# ") < 0)
            return;
        if (readline(S, S->line, LINEMAX) < 0)
            return;                     /* the caller hung up */

        char **argv = S->argv;
        int argc = split(S->line, argv, 8);
        if (argc == 0)
            continue;

        if (streq(argv[0], "exit") || streq(argv[0], "quit")) {
            puts_conn(S, "goodbye\n");
            return;
        }
        if (streq(argv[0], "help")) {
            do_help(S);
        } else if (streq(argv[0], "echo")) {
            /* The one command kept, because its output has to arrive *here*
               rather than wherever /dev/console points — which for a program
               started from this shell is here anyway, but for `echo` there is
               nothing to start. */
            for (int i = 1; i < argc; i++) {
                if (i > 1)
                    puts_conn(S, " ");
                puts_conn(S, argv[i]);
            }
            puts_conn(S, "\n");
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
                puts_conn(S, mnt ? "usage: mount [-a|-b] <prefix> <task>\n"
                              : "usage: bind [-a|-b] <old> <new>\n");
            } else if (mnt) {
                int t = 0;
                const char *d = argv[a + 1];
                while (*d >= '0' && *d <= '9')
                    t = t * 10 + (*d++ - '0');
                if (sys_mount(argv[a], t, f) < 0)
                    puts_conn(S, "mount: no room in the mount table\n");
            } else if (sys_bind(argv[a], argv[a + 1], f) < 0) {
                puts_conn(S, "bind: no room in the mount table\n");
            }
        } else if (streq(argv[0], "unmount")) {
            if (argc < 2)
                puts_conn(S, "usage: unmount <name>\n");
            else if (sys_unmount(argv[1]) < 0)
                puts_conn(S, "unmount: nothing bound there\n");
        } else if (streq(argv[0], "import")) {
            /* Two steps and nothing else: start the proxy, and mount it. It
               is a task, and a mount takes a task — the namespace has no
               notion of "remote" and does not need one. */
            if (argc < 4) {
                puts_conn(S, "usage: import <a.b.c.d> <port> <prefix>\n");
            } else {
                char *av[4];
                av[0] = (char *)"/BIN/IMPORT.ELF";
                av[1] = argv[1];
                av[2] = argv[2];
                av[3] = argv[3];        /* its own mount point, to strip */
                int tid = spawn(av[0], 4, av);
                if (tid < 0) {
                    puts_conn(S, "import: cannot run /BIN/IMPORT.ELF\n");
                } else if (sys_mount(argv[3], tid, MREPL) < 0) {
                    puts_conn(S, "import: no room in the mount table\n");
                } else {
                    puts_conn(S, "mounted, task ");
                    put_num(S, (unsigned long)tid);
                    puts_conn(S, "\n");
                }
            }
        } else {
            run_command(S, argc, argv);
        }
    }
}

/* Нить сессии. Первое, что она делает, — заводит собственное пространство
   имён: нити делят память, но не обязаны делить смысл имён, и именно поэтому
   две сессии могут привязать /dev/console каждая к своему соединению. Всё,
   что запустит эта сессия, унаследует её пространство, а не соседкино. */
static void session_thread(long arg)
{
    struct session *S = (struct session *)arg;

    /* Соединение открывает та задача, которая его и закроет.

       Держателя соединение помнит по идентификатору задачи, а нить — это
       отдельная задача. Открыть в родителе и закрыть в нити значит добавить
       ссылку одному и снять её у другого: ref_drop не находит совпадения,
       счётчик не доходит до нуля, и соединение навсегда остаётся в
       close-wait. Ровно это и случилось при первой сборке — три висящих
       соединения при пяти вошедших и вышедших. */
    S->conn = vfs_open(S->scratch);
    if (S->conn < 0) {
        char *st = S->stack;
        free(S);
        free(st);
        sys_exit();
    }

    /* Своё пространство имён: нити делят память, но не обязаны делить смысл
       имён — потому две сессии и могут привязать /dev/console каждая к своему
       соединению. Всё, что запустит эта сессия, унаследует её пространство. */
    sys_nsclone();
    sys_bind(S->scratch, "/dev/console", MREPL);

    session(S);

    vfs_close(S->conn);                 /* последний дескриптор: и соединение */
    uputs("  [rsh] they logged out\n");
    char *stack = S->stack;
    free(S);
    free(stack);                        /* стек, на котором мы стоим, — но
                                           free только помечает его свободным,
                                           а следующим действием мы уходим */
    sys_exit();
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
        /* Слот теперь может быть трёхзначным: соединений сто двадцать
           восемь, а не четыре. */
        k += uutoa((unsigned long)lslot, cmd + k);
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
        k += uutoa((unsigned long)slot, path + k);
        path[k] = 0;

        /* Одна сессия — одна нить. Всё, что ей нужно, выделяется здесь:
           состояние и стек. Освобождает она их сама, потому что этот цикл к
           тому времени уже ждёт следующего соединения. */
        struct session *S = malloc(sizeof(*S));
        char *stack = S ? malloc(SESSION_STACK) : 0;
        if (!S || !stack) {
            free(S);
            free(stack);
            uputs("  [rsh] no memory for a session\n");
            continue;
        }
        umemset(S, 0, sizeof(*S));
        S->stack = stack;
        S->slot  = slot;
        S->conn  = -1;                  /* откроет нить, и вот почему */
        for (int q = 0; q < (int)sizeof(path) && path[q]; q++)
            S->scratch[q] = path[q];

        int tid = sys_thread(session_thread, stack + SESSION_STACK, (long)S);
        if (tid < 0) {
            uputs("  [rsh] no task slot for a session\n");
            free(stack);
            free(S);
            continue;
        }
        uputs("  [rsh] someone logged in\n");
    }

    uputs("  [rsh] stopping\n");
    sys_exit();
}
