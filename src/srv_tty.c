/* srv_tty.c — the line discipline, as a server.

   Special characters in this system have always been handled by whoever
   reads: the local shell parsed its own input, the remote shell parsed its
   own, curses parsed a third time. Telnet's negotiation was decoded in two
   places independently and backspace in three, and the interrupt character
   was implemented twice — in the console server and in the network server —
   which is enough copies for them to have already drifted apart.

   The reason there was nowhere else to put it: between a driver and the
   program reading from it there is nothing at all. In Unix that nothing is
   the line discipline. Here it was missing because a terminal is not a type
   but a name — whatever /dev/console resolves to. This is that missing
   middle, and being a server rather than a layer inside the kernel is what
   lets it be *optional*: a program that does not want it binds /dev/console
   straight to the device.

     /tty/ctl        write "new /net/tcp/0"  -> "ok 3"
                     write "close 3"
     /tty/3          read  — a whole line, editing already done
                     write — passes through to the device

   ---- why this needed threads -------------------------------------------

   A server cannot block reading from another server: it would stop answering
   everyone else. This is exactly what killed the first attempt at a
   pseudo-terminal, and it was not a bug in the code — the mechanism was
   missing. Each line now gets a thread that blocks in vfs_read on the device
   and runs the discipline; when a line is complete it wakes the main loop,
   and the *main loop* answers the parked reader.

   That last part is not a detail. The client is waiting in a closed receive,
   which names the task it sent to — the main one, not the thread. A reply
   from the thread would sit in the queue unclaimed for ever. */
#include "vfs.h"
#include "servers.h"
#include "syscall.h"
#include "ulib.h"
#include "malloc.h"

#define NLINE     8             /* terminals at once */
#define TTY_LINE  256           /* the longest line that can be typed */
#define TTY_QUEUE 1024          /* what has been finished and not yet read */
#define TTY_STACK 16384

enum { M_COOKED = 0, M_RAW };

/* telnet, which arrives mixed into the same stream as the typing */
#define IAC 255
#define SEo 240
#define SBo 250
#define WILL 251
#define WONT 252
#define DO   253
#define DONT 254
enum { T_DATA, T_IAC, T_OPT, T_SB, T_SB_IAC };

struct line {
    int   used;
    int   raw;                  /* the device, as a descriptor */
    int   owner;                /* who asked for it, so a dead one is noticed */
    int   mode;
    int   dead;                 /* the device is gone; reads answer 0 */

    char  buf[TTY_LINE];        /* the line being typed */
    int   len;

    char  q[TTY_QUEUE];         /* finished, waiting to be read */
    int   qlen;

    int   waiter;               /* a reader parked on it, or -1 */
    struct vfs_req req;         /* their request, kept to answer later */

    int   intr_task;            /* whom to kill on the interrupt character */
    int   intr_fired;

    int   tstate;
    unsigned tcmd;

    int   thread;
    char *stack;
};

static struct line lines[NLINE];
static int  tty_lock_word;
static int  main_task;

/* One lock for the server, because the reader threads and the main loop touch
   the same lines. Same shape as the allocator's: an atomic swap, and waiting
   yields rather than spins, because the holder is a task the scheduler has to
   get back to. */
static void tty_lock(void)
{
    while (__sync_lock_test_and_set(&tty_lock_word, 1))
        sys_yield();
}

static void tty_unlock(void)
{
    __sync_lock_release(&tty_lock_word);
}

/* ---- the device -------------------------------------------------------- */

static void dev_write(struct line *L, const char *s, int n)
{
    int off = 0;
    while (off < n) {
        int k = vfs_write(L->raw, s + off, n - off);
        if (k < 0) {
            L->dead = 1;
            return;
        }
        if (k == 0) {
            sys_yield();
            continue;
        }
        off += k;
    }
}

static void echo(struct line *L, const char *s)
{
    int n = (int)ustrlen(s);
    dev_write(L, s, n);
}

/* ---- the discipline ---------------------------------------------------- */

static void q_put(struct line *L, const char *s, int n)
{
    if (L->qlen + n > TTY_QUEUE)
        n = TTY_QUEUE - L->qlen;
    if (n <= 0)
        return;
    umemcpy(L->q + L->qlen, s, (unsigned long)n);
    L->qlen += n;
}

/* A finished line goes into the queue with the newline a reader expects. */
static void line_done(struct line *L)
{
    q_put(L, L->buf, L->len);
    q_put(L, "\n", 1);
    L->len = 0;
    echo(L, "\r\n");
}

static void erase_one(struct line *L)
{
    if (!L->len)
        return;
    L->len--;
    /* Back over it, paint a space, back again — the only way to unprint a
       character on a terminal that has no idea what it drew. */
    echo(L, "\b \b");
}

static void erase_word(struct line *L)
{
    while (L->len && L->buf[L->len - 1] == ' ')
        erase_one(L);
    while (L->len && L->buf[L->len - 1] != ' ')
        erase_one(L);
}

/* One byte from the device. Returns 1 if something became readable. */
static int feed_byte(struct line *L, unsigned c)
{
    /* Telnet first, and in both modes: a negotiation is not typing, and a
       program that asked for raw bytes did not ask for these. */
    switch (L->tstate) {
    case T_IAC:
        if (c == IAC) {
            L->tstate = T_DATA;         /* two of them mean one of them */
            break;                      /* fall through to the data path */
        } else if (c == WILL || c == WONT || c == DO || c == DONT) {
            L->tcmd = c;
            L->tstate = T_OPT;
            return 0;
        } else if (c == SBo) {
            L->tstate = T_SB;
            return 0;
        } else {
            L->tstate = T_DATA;         /* NOP, AYT, break: nothing to do */
            return 0;
        }
    case T_OPT:
        if (L->tcmd == WILL || L->tcmd == DO) {
            char r[3];
            r[0] = (char)IAC;
            r[1] = (char)(L->tcmd == WILL ? DONT : WONT);
            r[2] = (char)c;
            dev_write(L, r, 3);         /* a refusal needs no answer */
        }
        L->tstate = T_DATA;
        return 0;
    case T_SB:
        if (c == IAC)
            L->tstate = T_SB_IAC;
        return 0;
    case T_SB_IAC:
        L->tstate = (c == SEo) ? T_DATA : T_SB;
        return 0;
    default:
        if (c == IAC) {
            L->tstate = T_IAC;
            return 0;
        }
    }

    /* The interrupt character, in both modes: the nomination exists only
       while a shell is waiting for a child, and that child is as likely to
       be full-screen as not. Acted on and swallowed — a program about to be
       killed is not going to read it. */
    if (c == 3 && L->intr_task > 0) {
        if (sys_alive(L->intr_task))
            sys_kill(L->intr_task);
        L->intr_task  = 0;
        L->intr_fired = 1;
        echo(L, "^C\r\n");
        L->len = 0;
        return 0;
    }

    if (L->mode == M_RAW) {
        char b = (char)c;
        q_put(L, &b, 1);
        return 1;                       /* raw: every byte is readable */
    }

    switch (c) {
    case '\r':
    case '\n':
        line_done(L);
        return 1;
    case 0:
        return 0;                       /* CR NUL is one carriage return */
    case 3:
        /* Nobody nominated: it means forget the line, which is what it means
           to a shell that is the one doing the reading. */
        while (L->len)
            erase_one(L);
        echo(L, "^C\r\n");
        return 0;
    case 4:                             /* ^D */
        if (L->len) {
            q_put(L, L->buf, L->len);   /* a partial line is delivered */
            L->len = 0;
            return 1;
        }
        L->dead = 1;                    /* on an empty line: end of file */
        return 1;
    case 8:
    case 127:
        erase_one(L);
        return 0;
    case 21:                            /* ^U */
        while (L->len)
            erase_one(L);
        return 0;
    case 23:                            /* ^W */
        erase_word(L);
        return 0;
    default:
        if (c >= 32 && L->len < TTY_LINE - 1) {
            char b = (char)c;
            L->buf[L->len++] = b;
            dev_write(L, &b, 1);        /* echo */
        }
        return 0;
    }
}

/* ---- the reader thread ------------------------------------------------- */

/* Blocks on the device so that the main loop never has to. Everything it
   touches is under the lock; when something becomes readable it pokes the
   main loop, which is the only task the waiting client will accept an answer
   from. */
static void reader(long arg)
{
    struct line *L = (struct line *)arg;

    for (;;) {
        char raw[VFS_DATA_MAX];
        int n = vfs_read(L->raw, raw, (int)sizeof(raw));
        int wake = 0;

        tty_lock();
        if (n <= 0) {
            /* The far end hung up. Whoever was in front of this terminal is
               now writing into nothing and reading from nothing, so it goes
               the same way the interrupt character would send it — this is
               what SIGHUP is for, and the nomination a shell already makes is
               exactly the list of who to tell. Without it a program left
               running by a dropped connection keeps running for ever, and the
               next session finds its output mixed into theirs. */
            if (L->intr_task > 0 && sys_alive(L->intr_task))
                sys_kill(L->intr_task);
            L->intr_task = 0;
            L->dead = 1;
            wake = 1;
        } else {
            for (int i = 0; i < n; i++)
                wake |= feed_byte(L, (unsigned char)raw[i]);
        }
        int done = L->dead;
        tty_unlock();

        if (wake) {
            unsigned long poke = 1;
            sys_send(main_task, &poke, (int)sizeof(poke));
        }
        if (done)
            break;
    }
    sys_exit();
}

/* ---- lines ------------------------------------------------------------- */

static int line_new(const char *path, int owner)
{
    tty_lock();
    int i = -1;
    for (int k = 0; k < NLINE; k++)
        if (!lines[k].used) { i = k; break; }
    /* Reclaim one whose creator is gone, the way every other server here
       does when it runs out. */
    if (i < 0)
        for (int k = 0; k < NLINE; k++)
            if (lines[k].used && !sys_alive(lines[k].owner)) { i = k; break; }
    tty_unlock();
    if (i < 0)
        return -1;

    struct line *L = &lines[i];
    /* Стек нити переживает линию и достаётся следующей в этом слоте.

       Освободить его негде: закрывающий линию не может отдать память, на
       которой в этот миг стоит чужая нить, а сама нить не может отдать свой
       стек и продолжить с него работать. Восемь слотов по шестнадцать
       килобайт — потолок, и он же весь расход. Первая версия просто не
       освобождала его вовсе, и каждая сессия уносила четыре страницы. */
    char *keep = L->stack;
    umemset(L, 0, sizeof(*L));
    L->stack  = keep;
    L->waiter = -1;
    L->mode   = M_COOKED;
    L->owner  = owner;
    L->raw    = vfs_open(path);
    if (L->raw < 0)
        return -1;

    if (!L->stack)
        L->stack = malloc(TTY_STACK);
    if (!L->stack) {
        vfs_close(L->raw);
        return -1;
    }
    L->used = 1;
    L->thread = sys_thread(reader, L->stack + TTY_STACK, (long)L);
    if (L->thread < 0) {
        vfs_close(L->raw);              /* стек остаётся слоту */
        L->used = 0;
        return -1;
    }
    return i;
}

static void line_close(struct line *L)
{
    if (!L->used)
        return;
    L->dead = 1;
    vfs_close(L->raw);              /* the thread's read fails and it exits */
    L->used = 0;
}

/* ---- answering --------------------------------------------------------- */

/* Take what is waiting. Zero when the device is gone, which is what a reader
   at end of file has to be told. */
static int take(struct line *L, struct vfs_req *r)
{
    int n = L->qlen;
    /* At most one line, so that a reader which asked for a command gets a
       command. Two lines typed quickly are two reads, not one string with a
       newline in the middle for the caller to find. */
    if (L->mode == M_COOKED)
        for (int i = 0; i < L->qlen; i++)
            if (L->q[i] == '\n') { n = i + 1; break; }
    if (n > r->len)
        n = r->len;
    if (n > 0) {
        umemcpy(r->data, L->q, (unsigned long)n);
        for (int i = n; i < L->qlen; i++)
            L->q[i - n] = L->q[i];
        L->qlen -= n;
    }
    return n;
}

/* Answer whoever is parked, if there is anything to say. trysend, not send:
   the client may be busy with something else of ours, and a server that
   blocks on a client is a deadlock waiting for an excuse. */
static void serve_parked(void)
{
    for (int i = 0; i < NLINE; i++) {
        struct line *L = &lines[i];
        if (!L->used || L->waiter < 0)
            continue;
        if (!L->qlen && !L->dead)
            continue;
        if (!sys_alive(L->waiter)) {
            L->waiter = -1;
            continue;
        }
        L->req.result = take(L, &L->req);
        if (sys_trysend(L->waiter, &L->req, (int)sizeof(L->req)) == 0)
            L->waiter = -1;
        /* If the client was busy the request stays parked and this runs
           again on the next poke. */
    }
}

/* ---- the files --------------------------------------------------------- */

/* An open control file or directory listing. It needs a position of its own,
   which the first version forgot: a read that always answers with the whole
   listing never reaches the end, and `ls` reads until it gets nothing. The
   shell hung on the first `ls /tty` — the same slot-per-open shape the
   network server uses for /net/ctl is what this is. */
#define NPF 4

struct pfile {
    int  used;
    int  owner;
    char buf[256];
    int  len, pos;
};
static struct pfile pf[NPF];

enum { FD_PF0 = 1, FD_LINE0 = 16 };

static int pf_alloc(int owner)
{
    for (int i = 0; i < NPF; i++)
        if (!pf[i].used) {
            pf[i].used = 1;
            pf[i].owner = owner;
            pf[i].len = pf[i].pos = 0;
            return i;
        }
    for (int i = 0; i < NPF; i++)
        if (pf[i].used && !sys_alive(pf[i].owner)) {
            pf[i].owner = owner;
            pf[i].len = pf[i].pos = 0;
            return i;
        }
    return -1;
}

static int is_name(const char *p, const char *name)
{
    int i = 0;
    while (name[i] && p[i] == name[i])
        i++;
    if (name[i])
        return 0;
    return p[i] == 0 || (p[i] == '/' && p[i + 1] == 0);
}

static int path_line(const char *p)
{
    const char *s = p + ustrlen("/tty/");
    if (*s < '0' || *s > '9')
        return -1;
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v < NLINE ? v : -1;
}

static int append(char *out, int o, const char *s)
{
    int l = (int)ustrlen(s);
    umemcpy(out + o, s, (unsigned long)l);
    return o + l;
}

static int format_dir(char *out, int cap)
{
    int o = append(out, 0, "- 0 ctl\n");
    for (int i = 0; i < NLINE && o < cap - 16; i++) {
        if (!lines[i].used)
            continue;
        o = append(out, o, "- 0 ");
        o += uutoa((unsigned long)i, out + o);
        out[o++] = '\n';
    }
    return o;
}

/* "new <path>" and "close <n>", answered in the same message — the shape
   /net/ctl established, because a control file that answers is easier to use
   from a shell than one that does not. */
static int do_ctl(const char *cmd, int from, char *out, int cap)
{
    (void)cap;
    if (cmd[0] == 'n' && cmd[1] == 'e' && cmd[2] == 'w') {
        const char *p = cmd + 3;
        while (*p == ' ')
            p++;
        char path[VFS_PATH_MAX];
        int k = 0;
        while (*p && *p != '\n' && k < VFS_PATH_MAX - 1)
            path[k++] = *p++;
        path[k] = 0;
        int i = line_new(path, from);
        if (i < 0)
            return append(out, 0, "error no line\n");
        int o = append(out, 0, "ok ");
        o += uutoa((unsigned long)i, out + o);
        out[o++] = '\n';
        return o;
    }
    if (cmd[0] == 'c' && cmd[1] == 'l') {
        const char *p = cmd + 5;
        while (*p == ' ')
            p++;
        int v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (*p++ - '0');
        if (v < 0 || v >= NLINE || !lines[v].used)
            return append(out, 0, "error no such line\n");
        line_close(&lines[v]);
        return append(out, 0, "ok\n");
    }
    return append(out, 0, "error syntax\n");
}

static const char tty_doc[] =
    "tty — the line discipline, as a server.\n"
    "\n"
    "ctl       write a line, read the answer:\n"
    "            new <path>   -> ok <n>   wrap that device in a terminal\n"
    "            close <n>\n"
    "<n>       read  — a whole line, with the editing already done\n"
    "          write — passes through to the device underneath\n"
    "\n"
    "What it understands while reading:\n"
    "  backspace and delete, ^U kill line, ^W kill word,\n"
    "  ^D end of file (a partial line is delivered first),\n"
    "  ^C kill the nominated task — acted on and swallowed,\n"
    "  CR, LF and CR NUL all end a line,\n"
    "  and telnet's IAC negotiation, which is refused.\n"
    "\n"
    "Everything above used to be written three times, in the two shells and\n"
    "in curses, and the interrupt character twice more in the two drivers.\n"
    "\n"
    "ioctl     INTR     a task id: kill it on the interrupt character\n"
    "          TTYMODE  1 = raw, 0 = cooked. Raw gives bytes as they\n"
    "                   arrive, with no echo and no editing, which is what\n"
    "                   a full-screen program wants\n"
    "          PING, DOC, CONF\n"
    "\n"
    "Bound over /dev/console it is invisible to whoever reads; bypassed by\n"
    "binding the device there instead. That choice belongs to whoever starts\n"
    "the program, which is the whole reason this is a server and not a\n"
    "library.\n";

static const char tty_conf[] =
    "lines        8    each with a thread of its own reading its device\n"
    "line         256  bytes, the longest that can be typed\n"
    "queue        1024 bytes finished and not yet read\n"
    "thread stack 16384 bytes\n";

void tty_server(void)
{
    main_task = sys_self();
    for (int i = 0; i < NLINE; i++)
        lines[i].waiter = -1;

    uputs("  [tty] up (user mode), a line discipline for whoever binds it\n");

    for (;;) {
        struct vfs_req req;
        int from = sys_recv(&req, (int)sizeof(req));
        struct vfs_req *r = &req;

        /* A poke from one of our own threads: something became readable. The
           sender tells us which kind of message this is, because the payload
           of a poke is nothing at all. */
        int is_thread = 0;
        for (int i = 0; i < NLINE; i++)
            if (lines[i].used && lines[i].thread == from)
                is_thread = 1;
        if (is_thread) {
            tty_lock();
            serve_parked();
            tty_unlock();
            continue;
        }

        switch (r->op) {
        case VFS_OPEN:
            if (is_name(r->path, "/tty") || ustr_has_prefix(r->path, "/tty/ctl")) {
                int i = pf_alloc(from);
                if (i < 0) {
                    r->result = -1;
                    break;
                }
                /* The directory is rendered once, at open, so that reading it
                   twice does not change it — the same rule /net/status
                   follows and for the same reason. */
                if (is_name(r->path, "/tty")) {
                    tty_lock();
                    pf[i].len = format_dir(pf[i].buf, (int)sizeof(pf[i].buf));
                    tty_unlock();
                }
                r->result = FD_PF0 + i;
            } else {
                int i = path_line(r->path);
                tty_lock();
                int ok = i >= 0 && lines[i].used;
                tty_unlock();
                r->result = ok ? FD_LINE0 + i : -1;
            }
            break;

        case VFS_READ:
            if (r->fd >= FD_PF0 && r->fd < FD_PF0 + NPF) {
                struct pfile *f = &pf[r->fd - FD_PF0];
                int n = f->len - f->pos;
                if (n > r->len) n = r->len;
                if (n < 0) n = 0;
                umemcpy(r->data, f->buf + f->pos, (unsigned long)n);
                f->pos += n;
                r->result = n;
                break;
            }
            {
                int i = r->fd - FD_LINE0;
                if (i < 0 || i >= NLINE || !lines[i].used) {
                    r->result = -1;
                    break;
                }
                struct line *L = &lines[i];
                tty_lock();
                if (L->qlen || L->dead) {
                    r->result = take(L, r);
                    tty_unlock();
                    break;
                }
                /* Nothing typed yet. Keep the request and answer when the
                   thread says there is something — blocking is a server
                   declining to answer yet. */
                L->waiter = from;
                L->req = *r;
                tty_unlock();
                continue;               /* no reply, on purpose */
            }

        case VFS_WRITE:
            if (r->fd >= FD_PF0 && r->fd < FD_PF0 + NPF) {
                struct pfile *f = &pf[r->fd - FD_PF0];
                f->len = do_ctl(r->data, from, f->buf, (int)sizeof(f->buf));
                f->pos = 0;
                umemcpy(r->data, f->buf, (unsigned long)f->len);
                r->result = f->len;     /* the answer rides home in the write */
                break;
            }
            {
                int i = r->fd - FD_LINE0;
                if (i < 0 || i >= NLINE || !lines[i].used) {
                    r->result = -1;
                    break;
                }
                /* Straight through. This is the round trip the discipline
                   costs on output, and there is no way around it: a
                   descriptor names one server. */
                dev_write(&lines[i], r->data, r->len);
                r->result = r->len;
            }
            break;

        case VFS_IOCTL:
            if (r->ioctl_cmd == IOCTL_PING) {
                r->result = 0;
            } else if (r->ioctl_cmd == IOCTL_DOC) {
                r->result = vfs_doc_reply(r, tty_doc);
            } else if (r->ioctl_cmd == IOCTL_CONF) {
                r->result = vfs_doc_reply(r, tty_conf);
            } else {
                int i = r->fd - FD_LINE0;
                if (i < 0 || i >= NLINE || !lines[i].used) {
                    r->result = -1;
                    break;
                }
                struct line *L = &lines[i];
                tty_lock();
                if (r->ioctl_cmd == IOCTL_INTR) {
                    r->result = r->len > 0 ? 0 : L->intr_fired;
                    L->intr_task  = r->len > 0 ? r->len : 0;
                    L->intr_fired = 0;
                } else if (r->ioctl_cmd == IOCTL_TTYMODE) {
                    L->mode = r->len ? M_RAW : M_COOKED;
                    L->len  = 0;
                    r->result = 0;
                } else {
                    r->result = -1;
                }
                tty_unlock();
            }
            break;

        case VFS_CLOSE:
            /* A descriptor on a line is closed; the line is not. A program
               that has finished with its console — mc does exactly this on the
               way out — would otherwise tear down the terminal underneath the
               shell that started it, and the session would die with it. The
               line goes when `close N` is written to ctl, when its device
               ends, or when the task that asked for it is gone. */
            if (r->fd >= FD_PF0 && r->fd < FD_PF0 + NPF)
                pf[r->fd - FD_PF0].used = 0;
            r->result = 0;
            break;

        default:
            r->result = -1;
            break;
        }
        sys_send(from, r, (int)sizeof(*r));
    }
}
