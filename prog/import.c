/* import.c — mount another machine's namespace into this one.

   Plan 9's import, and the shape is the same: this program holds a TCP
   connection to an exportfs somewhere, and is itself mounted at a prefix. A
   program that opens a name under that prefix sends a request to *this* task,
   exactly as it would to the filesystem or the console server; this one
   forwards it down the connection and hands back what comes up. Nothing in
   the namespace knows the difference — a mount takes a task, and a task is
   what this is.

   The interesting part is not the forwarding. It is that this is the first
   task in the system with two things outstanding at once: a read parked on
   the network, and a request in flight to the same server. The two directions
   can collide, which is why sys_trysend had to exist — see the README.

   It follows that this program cannot use vfs_read and vfs_write. Those send
   and then receive, and assume the next message is the answer; here the next
   message may be a client arriving, or the parked read completing, or the
   reply being waited for. So the loop is written on raw send and recv, and
   dispatches on who sent it and what it is.

   The prefix has to be an argument, and that is worth a word. Every server
   here is handed the *whole* path and knows its own mount point: the
   filesystem strips a leading slash, the proc server matches "/proc/". A
   proxy cannot know its own mount point — somebody else mounts it — so it is
   told, and strips it, so that the far end is asked about a name in its own
   tree rather than in ours.

   usage: /IMPORT.ELF <a.b.c.d> <port> <prefix>    (mounted by the shell)   */
#include "syscall.h"
#include "vfs.h"
#include "fsproto.h"

#define NPEND 4

static int ilen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}
static void say(const char *s) { vfs_say(s); }

/* Its own, because ulib is linked into the shared user text of kernel.elf and
   a program loaded off the disk cannot reach it. */
static void icpy(void *d, const void *s, int n)
{
    char *a = d; const char *b = s;
    while (n-- > 0) *a++ = *b++;
}
static void izero(void *d, int n)
{
    char *a = d;
    while (n-- > 0) *a++ = 0;
}

/* ---- the connection ---------------------------------------------------- */

static int conn = -1;        /* packed: (net server << 16) | its descriptor */
static int netsrv;

static unsigned char inbuf[FS_MAXMSG * 2];
static int inlen;

static unsigned char outq[FS_MAXMSG * 2];
static int outlen;
static int write_busy;       /* a write request is in flight to the network */
static int read_posted;

/* A request forwarded and not yet answered: who to give the answer to. */
static struct {
    int used;
    unsigned tag;
    int client;
    int fd;                  /* the client's own descriptor, echoed back */
    int op;
} pend[NPEND];

static unsigned next_tag = 1;
static const char *prefix = "/";

/* ---- talking to the network server, without vfs_read/vfs_write ---------- */

static void post_read(void)
{
    if (read_posted || conn < 0)
        return;
    struct vfs_req r;
    izero(&r, (int)sizeof(r));
    r.op  = VFS_READ;
    r.fd  = conn & 0xffff;
    r.len = VFS_DATA_MAX;
    if (sys_send(netsrv, &r, (int)sizeof(r)) < 0)
        return;
    read_posted = 1;
}

static void kick_write(void)
{
    if (write_busy || outlen == 0 || conn < 0)
        return;
    struct vfs_req r;
    izero(&r, (int)sizeof(r));
    r.op  = VFS_WRITE;
    r.fd  = conn & 0xffff;
    r.len = outlen < VFS_DATA_MAX ? outlen : VFS_DATA_MAX;
    icpy(r.data, outq, r.len);
    if (sys_send(netsrv, &r, (int)sizeof(r)) < 0)
        return;
    write_busy = 1;
}

static void write_done(int n)
{
    write_busy = 0;
    if (n <= 0)
        return;                         /* nothing moved: try the same bytes */
    if (n > outlen)
        n = outlen;
    outlen -= n;
    for (int i = 0; i < outlen; i++)
        outq[i] = outq[i + n];
}

static int queue(const struct fs_msg *m)
{
    unsigned char tmp[FS_MAXMSG];
    int n = fs_encode(m, tmp, (int)sizeof(tmp));
    if (n < 0 || outlen + n > (int)sizeof(outq))
        return -1;                      /* the far end is not keeping up */
    for (int i = 0; i < n; i++)
        outq[outlen + i] = tmp[i];
    outlen += n;
    kick_write();
    return 0;
}

/* ---- clients ----------------------------------------------------------- */

static void answer(int client, int op, int fd, int result,
                   const char *data, int n)
{
    struct vfs_req r;
    izero(&r, (int)sizeof(r));
    r.op     = op;
    r.fd     = fd;
    r.result = result;
    if (n > 0)
        icpy(r.data, data, n);
    sys_send(client, &r, (int)sizeof(r));
}

static void from_client(int who, struct vfs_req *r)
{
    int slot = -1;
    for (int i = 0; i < NPEND; i++)
        if (!pend[i].used) {
            slot = i;
            break;
        }
    if (slot < 0 || conn < 0) {
        answer(who, r->op, r->fd, -1, 0, 0);
        return;
    }

    struct fs_msg m;
    izero(&m, (int)sizeof(m));
    m.tag   = next_tag++ & 0xffff;
    m.kind  = FS_REQUEST;
    m.op    = r->op;
    m.fd    = r->fd;
    m.count = r->len;
    m.ioctl = r->ioctl_cmd;
    if (r->op == VFS_OPEN) {
        /* "/r/README.TXT" under a mount at "/r/" is "/README.TXT" over there. */
        const char *q = r->path;
        const char *pp = prefix;
        while (*pp && *q == *pp) {
            q++;
            pp++;
        }
        if (*pp)                        /* not actually under our prefix */
            q = r->path;
        int n = 0;
        if (*q != '/')
            m.path[n++] = '/';
        while (*q && n < VFS_PATH_MAX)
            m.path[n++] = *q++;
        m.pathlen = n;
    }
    if (r->op == VFS_WRITE) {
        int n = r->len < VFS_DATA_MAX ? r->len : VFS_DATA_MAX;
        if (n > 0)
            icpy(m.data, r->data, n);
        m.datalen = n;
    }

    if (queue(&m) < 0) {
        answer(who, r->op, r->fd, -1, 0, 0);
        return;
    }
    pend[slot].used   = 1;
    pend[slot].tag    = m.tag;
    pend[slot].client = who;
    pend[slot].fd     = r->fd;
    pend[slot].op     = r->op;
}

/* One reply off the wire: find whose it was and hand it over. */
static void from_wire(const struct fs_msg *m)
{
    for (int i = 0; i < NPEND; i++) {
        if (!pend[i].used || pend[i].tag != m->tag)
            continue;
        pend[i].used = 0;
        answer(pend[i].client, pend[i].op, pend[i].fd, m->result,
               m->data, m->datalen);
        return;
    }
    /* A tag nobody is waiting for. Dropping it is right: the alternative is
       to guess, and a protocol that guesses is worse than one that loses. */
}

static void drain_in(void)
{
    for (;;) {
        struct fs_msg m;
        int used = fs_decode(inbuf, inlen, &m);
        if (used == 0)
            return;                     /* not a whole message yet */
        if (used < 0) {
            say("  [import] malformed reply; hanging up\n");
            inlen = 0;
            conn = -1;
            return;
        }
        for (int i = used; i < inlen; i++)
            inbuf[i - used] = inbuf[i];
        inlen -= used;
        from_wire(&m);
    }
}

/* ---- setting up -------------------------------------------------------- */

static int ctl(const char *cmd, char *answerbuf, int cap)
{
    int fd = vfs_open("/net/ctl");
    if (fd < 0)
        return -1;
    if (vfs_write(fd, cmd, ilen(cmd)) < 0) {
        vfs_close(fd);
        return -1;
    }
    int n = vfs_read(fd, answerbuf, cap - 1);
    vfs_close(fd);
    if (n <= 0)
        return -1;
    answerbuf[n] = 0;
    return n;
}

static const char *ok_value(const char *a)
{
    if (a[0] != 'o' || a[1] != 'k')
        return 0;
    const char *s = a + 2;
    while (*s == ' ')
        s++;
    return s;
}

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    if (argc < 3) {
        say("  [import] usage: /IMPORT.ELF <a.b.c.d> <port> [prefix]\n");
        sys_exit();
    }
    if (argc > 3)
        prefix = argv[3];

    char cmd[80], ans[80];
    char *p = cmd;
    const char *w = "connect ";
    while (*w)
        *p++ = *w++;
    for (const char *q = argv[1]; *q; q++)
        *p++ = *q;
    *p++ = ' ';
    for (const char *q = argv[2]; *q; q++)
        *p++ = *q;
    *p = 0;

    /* connect blocks until the handshake is done, so by the time this
       returns there is either a connection or a reason there is not. */
    if (ctl(cmd, ans, sizeof(ans)) < 0 || !ok_value(ans)) {
        say("  [import] ");
        say(ans);
        say("\n");
        sys_exit();
    }

    char path[24];
    p = path;
    w = "/net/tcp/";
    while (*w)
        *p++ = *w++;
    for (const char *q = ok_value(ans); *q >= '0' && *q <= '9'; q++)
        *p++ = *q;
    *p = 0;

    conn = vfs_open(path);
    if (conn < 0) {
        say("  [import] cannot open the connection\n");
        sys_exit();
    }
    netsrv = conn >> 16;
    say("  [import] connected; serving that namespace here\n");

    post_read();

    for (;;) {
        struct vfs_req req;
        int from = sys_recv(&req, (int)sizeof(req));

        if (from == netsrv) {
            /* The network server answers both of our outstanding things. Which
               one this is, is in the op it echoes back. */
            if (req.op == VFS_READ) {
                read_posted = 0;
                if (req.result <= 0) {
                    say("  [import] the far end hung up\n");
                    break;
                }
                int n = req.result;
                if (n > (int)sizeof(inbuf) - inlen)
                    n = (int)sizeof(inbuf) - inlen;
                icpy(inbuf + inlen, req.data, n);
                inlen += n;
                drain_in();
                if (conn < 0)
                    break;
                post_read();
            } else if (req.op == VFS_WRITE) {
                write_done(req.result);
                kick_write();
            }
            continue;
        }

        from_client(from, &req);
    }

    /* Whoever is still waiting gets an error rather than silence. */
    for (int i = 0; i < NPEND; i++)
        if (pend[i].used) {
            pend[i].used = 0;
            answer(pend[i].client, pend[i].op, pend[i].fd, -1, 0, 0);
        }
    if (conn >= 0)
        vfs_close(conn);
    say("  [import] stopping\n");
    sys_exit();
}
