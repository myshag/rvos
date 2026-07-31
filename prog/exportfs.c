/* exportfs.c — hand this machine's namespace to whoever connects.

   Plan 9's exportfs, in the only sense that matters here: it accepts a
   connection, reads file requests off it, performs them through its own
   namespace, and writes the answers back. What it exports is *its* view of
   the tree — not the machine's, because there is no such thing; the tree it
   can see is the tree the caller gets.

   Nothing in it knows what a remote machine is. It opens files by name and
   moves bytes, which is what every server in this system does; the only
   difference is that its client arrives over TCP instead of over IPC, and
   that difference is entirely inside the two dozen lines that read and write
   the connection.

   There is no authentication. Anybody who can reach the port has the
   namespace. That is a sentence, not an oversight — see the README.

   usage: /EXPORTFS.ELF [port]        (default 564, Plan 9's)               */
#include "syscall.h"
#include "vfs.h"
#include "fsproto.h"

#define DEFAULT_PORT 564
#define NFD 8

static int xlen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void say(const char *s) { vfs_say(s); }

/* The caller names files by numbers this program hands out, so they have to
   be small: a descriptor from vfs_open packs a server id into its top half
   and would not survive the trip. A table of its own, like every other
   server here keeps. */
static struct {
    int used;
    int fd;                             /* what our own namespace calls it */
} tab[NFD];

static int tab_alloc(int fd)
{
    for (int i = 0; i < NFD; i++)
        if (!tab[i].used) {
            tab[i].used = 1;
            tab[i].fd = fd;
            return i;
        }
    return -1;
}

/* ---- the connection ---------------------------------------------------- */

static int conn = -1;

static int wr_all(const unsigned char *p, int n)
{
    int off = 0;
    while (off < n) {
        int k = vfs_write(conn, p + off, n - off);
        if (k < 0)
            return -1;
        if (k == 0) {
            sys_yield();                /* the send buffer is full */
            continue;
        }
        off += k;
    }
    return 0;
}

static int ctl(const char *cmd, char *answer, int cap)
{
    int fd = vfs_open("/net/ctl");
    if (fd < 0)
        return -1;
    if (vfs_write(fd, cmd, xlen(cmd)) < 0) {
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

static void put_int(char *out, int *n, int v)
{
    char tmp[12];
    int t = 0;
    if (v == 0)
        tmp[t++] = '0';
    while (v) {
        tmp[t++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (t)
        out[(*n)++] = tmp[--t];
}

/* ---- one request ------------------------------------------------------- */

static void serve(struct fs_msg *m)
{
    m->kind   = FS_REPLY;
    m->result = -1;

    switch (m->op) {
    case VFS_OPEN:
    case VFS_CREATE: {
        m->path[m->pathlen < VFS_PATH_MAX ? m->pathlen : VFS_PATH_MAX - 1] = 0;
        int fd = m->op == VFS_CREATE ? vfs_create(m->path) : vfs_open(m->path);
        if (fd < 0)
            break;
        int h = tab_alloc(fd);
        if (h < 0) {
            vfs_close(fd);
            break;
        }
        m->result = h;
        break;
    }
    case VFS_READ: {
        if (m->fd < 0 || m->fd >= NFD || !tab[m->fd].used)
            break;
        int want = m->count;
        if (want > VFS_DATA_MAX)
            want = VFS_DATA_MAX;
        int n = vfs_read(tab[m->fd].fd, m->data, want);
        m->result  = n;
        m->datalen = n > 0 ? n : 0;
        break;
    }
    case VFS_WRITE: {
        if (m->fd < 0 || m->fd >= NFD || !tab[m->fd].used)
            break;
        m->result  = vfs_write(tab[m->fd].fd, m->data, m->datalen);
        m->datalen = 0;
        break;
    }
    case VFS_IOCTL:
        if (m->fd < 0 || m->fd >= NFD || !tab[m->fd].used)
            break;
        m->result = vfs_ioctl(tab[m->fd].fd, m->ioctl);
        break;
    case VFS_CLOSE:
        if (m->fd < 0 || m->fd >= NFD || !tab[m->fd].used)
            break;
        vfs_close(tab[m->fd].fd);
        tab[m->fd].used = 0;
        m->result = 0;
        break;
    default:
        break;
    }
    m->pathlen = 0;
}

__attribute__((section(".text.start"))) void _start(int argc, char **argv)
{
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = 0;
        for (const char *d = argv[1]; *d >= '0' && *d <= '9'; d++)
            port = port * 10 + (*d - '0');
    }

    char answer[64], cmd[40];
    int n = 0;
    const char *w = "listen ";
    while (*w)
        cmd[n++] = *w++;
    put_int(cmd, &n, port);
    cmd[n] = 0;

    if (ctl(cmd, answer, sizeof(answer)) < 0 || slot_of(answer) < 0) {
        say("  [exportfs] cannot take the port\n");
        sys_exit();
    }
    int lslot = slot_of(answer);
    say("  [exportfs] exporting this namespace on port ");
    { char b[12]; int k = 0; put_int(b, &k, port); b[k] = 0; say(b); }
    say("\n");

    for (;;) {
        n = 0;
        w = "accept ";
        while (*w)
            cmd[n++] = *w++;
        put_int(cmd, &n, lslot);
        cmd[n] = 0;
        if (ctl(cmd, answer, sizeof(answer)) < 0)
            break;
        int slot = slot_of(answer);
        if (slot < 0)
            break;

        char path[24];
        n = 0;
        w = "/net/tcp/";
        while (*w)
            path[n++] = *w++;
        put_int(path, &n, slot);
        path[n] = 0;

        conn = vfs_open(path);
        if (conn < 0)
            continue;
        say("  [exportfs] a client\n");

        /* TCP is a stream: a message may arrive in pieces, and two may arrive
           together. Everything not yet whole stays in the buffer. */
        unsigned char in[FS_MAXMSG * 2];
        unsigned char out[FS_MAXMSG];
        int have = 0;

        for (;;) {
            int k = vfs_read(conn, (char *)in + have, (int)sizeof(in) - have);
            if (k <= 0)
                break;                  /* 0 = the client closed its half */
            have += k;

            for (;;) {
                struct fs_msg m;
                int used = fs_decode(in, have, &m);
                if (used == 0)
                    break;              /* not a whole message yet */
                if (used < 0) {
                    say("  [exportfs] malformed request; dropping the client\n");
                    have = 0;
                    k = -1;
                    break;
                }
                for (int i = used; i < have; i++)
                    in[i - used] = in[i];
                have -= used;

                serve(&m);
                int len = fs_encode(&m, out, (int)sizeof(out));
                if (len < 0 || wr_all(out, len) < 0) {
                    k = -1;
                    break;
                }
            }
            if (k < 0)
                break;
        }

        for (int i = 0; i < NFD; i++)
            if (tab[i].used) {
                vfs_close(tab[i].fd);
                tab[i].used = 0;
            }
        vfs_close(conn);
        conn = -1;
        say("  [exportfs] client gone\n");
    }

    say("  [exportfs] stopping\n");
    sys_exit();
}
