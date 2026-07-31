#pragma once
#include "riscv.h"
#include "syscall.h"

/* vfs.h — the ONE interface every module speaks, inside the kernel or out:
   open / read / write / ioctl / close. Plan-9 style: "everything is a file".
   A directory answers read() with a text listing; a device answers write()
   with a side effect; ioctl() covers whatever doesn't fit read/write.

   Crucially this is NOT a kernel syscall set — the kernel only provides
   send/recv/yield (see syscall.h). open/read/write/ioctl/close is a userspace
   *protocol*: a shared struct plus a namespace, which any two tasks can speak
   on top of that raw IPC. That's the microkernel discipline: the kernel stays
   minimal; the interface is a library, not a privileged primitive. Adding a
   module (a pipe, a socket, /proc) never touches the kernel — it's just
   another task answering vfs_req, bound into the namespace at runtime. */

enum { VFS_OPEN = 1, VFS_READ, VFS_WRITE, VFS_IOCTL, VFS_CLOSE, VFS_CREATE };

/* create is its own operation rather than a flag on open, and the reason is
   that open has no flags and giving it some would mean every server growing
   an opinion about them. A server that cannot create anything answers -1 to
   this and nothing else changes. */

/* Generic ioctls. IOCTL_GETSIZE reports a file's size without slurping its
   bytes. New commands plug in here without changing the transport. */
enum {
    IOCTL_GETSIZE = 1,
    IOCTL_REMOVE,
    IOCTL_MKDIR,
    /* Answered by every server, immediately, with 0 and nothing else. It is
       the one request whose *timing* is the answer: how long a message takes
       to cross to a module and come back, and — when it does not come back —
       which module has stopped answering. A system made of servers should be
       able to say that about itself. */
    IOCTL_PING,
    /* "If the interrupt character arrives, kill this task." The argument is a
       task id, or 0 for nobody, and it travels in `len` because an ioctl has
       no other field to spare.

       This is what a tty driver does in Unix, minus the part this system does
       not have: there, the line discipline recognises INTR and sends SIGINT to
       the foreground process group. There are no signals here and no groups,
       so the terminal is told which single task is in front of it and kills
       that one. A shell sets it before it waits and clears it after. */
    IOCTL_INTR,
    /* "Describe yourself." A server answers with text: which ioctls it takes,
       which words its control file accepts, what its names mean. The answer
       is not a file on a disk anywhere — it is the server speaking, so it
       cannot be stale, and a server that has no answer has no documentation
       rather than wrong documentation.

       `len` in is an offset, `data` and `result` out are the bytes from
       there, so a description longer than one message is read in several.
       Like a ping, it is about the server and not about a descriptor, and is
       answered before the fd is looked at. */
    IOCTL_DOC,
    /* "What are you holding for task N?" — the id in `len`, the answer as
       text in `data`, one message. A task's open files are not in one place
       in this system: there is no client-side descriptor table, only each
       server's own record of who asked it for what. So the only way to ask
       what a task has open is to ask everybody, and this is the question. */
    IOCTL_HOLDS,
    /* "What numbers were you built with?" Answered like DOC, and separate
       from it because they are different kinds of fact: DOC is what a server
       accepts, this is what it can hold. */
    IOCTL_CONF,
    /* Сырой режим против варёного, на терминале: `len` = 1 сырой, 0 варёный.
       В сыром байты отдаются как пришли, без эха и без правки строки — это
       то, что нужно полноэкранной программе. */
    IOCTL_TTYMODE,
};

/* Long names need somewhere to fit. 32 was enough while every name was 8.3;
   a path like /DOCS/длинное имя.txt is 40 bytes of UTF-8 before it starts. */
#define VFS_PATH_MAX 128
#define VFS_DATA_MAX 512

/* The whole request travels by value. It used to carry a `void *buf` into
   the caller's memory, which worked only while every task shared one address
   space; with per-task page tables that pointer is meaningless on the far
   side, so payloads now ride inline and the kernel copies the struct across.
   The cost of isolation, made visible: reads are chunked to VFS_DATA_MAX. */
struct vfs_req {
    int    op;
    int    fd;                  /* IN: server-local fd, for read/write/ioctl/close */
    char   path[VFS_PATH_MAX];  /* IN: for open */
    int    len;                 /* IN: bytes requested / supplied */
    unsigned long ioctl_cmd;
    int    result;              /* OUT: bytes moved / local fd / size / -1 */
    char   data[VFS_DATA_MAX];  /* IN for write, OUT for read */
};

/* ---- namespace (vfs.c) ----------------------------------------------
   Paths are resolved through a mount table, not hardcoded prefixes: bind a
   prefix to a server task and that subtree is served by it, longest-prefix
   wins. The table is data, not policy — it can be rewritten on a running
   system — and it belongs to a *task*, not to the system: vfs_ns_clone()
   hands the caller a private copy it can diverge (Plan 9's rfork(RFNAMEG)),
   so the same path in two tasks may reach two different modules.

   Since the namespace is the caller's, a server asked to report one must be
   told whose — there is no ambient "the" namespace to inspect. */
#define VFS_PREFIX_MAX 16
#define VFS_NMOUNT     8
/* As many namespaces as there are tasks, because a namespace is reachable
   only through a task that is looking at it: more could never be wanted. The
   number matches NTASK in task.h, which this header cannot see — a user
   program includes this one and not that one. */
#define VFS_NNS        24       /* root + private namespaces */

struct namespace;

/* Plan 9's three: replace what is at that name, or join it — before what is
   there, or after. Joining is a union: the name then has several answers and
   a lookup tries them in order until one has what was asked for. */
enum { MREPL = 0, MBEFORE, MAFTER };

int vfs_mount(const char *prefix, int server_task, int flags);
int vfs_bind(const char *old, const char *new, int flags);
int vfs_unmount(const char *name);                 /* the whole union of it */
int vfs_ns_clone(void);                             /* private copy of it */
void vfs_ns_gc(void);        /* release namespaces no live task refers to */
int  vfs_ns_inuse(void);     /* how many of VFS_NNS are taken */
/* -> the server that answers for the `nth` member of whatever union is at
   that name, and in `out` the name to ask it about, which differs from `path`
   if a bind was crossed. -1 once the union has no nth member. */
int vfs_resolve(const char *path, char *out, int cap, int nth);
int vfs_dump_mounts_of(int task_id, char *out, int cap);
struct namespace *vfs_root_ns(void);

/* ---- client side ----------------------------------------------------
   A client fd packs which server owns it together with that server's local
   fd, so read/write/ioctl/close never have to re-resolve the path.

   Everything below reaches the outside world through syscalls only — no
   kernel function is called, no kernel variable is read. That is deliberate:
   the same header is the client library for a user-mode program, which is
   permitted nothing else. */
static inline void vfs__cpy(void *d, const void *s, int n)
{
    char *a = (char *)d; const char *b = (const char *)s;
    while (n-- > 0) *a++ = *b++;
}
static inline void vfs__scpy(char *d, const char *s)
{
    while ((*d++ = *s++)) ;
}
/* Request out, reply back into the same struct: the server works on its own
   copy, so the answer has to be shipped home explicitly. */
static inline int vfs_call(int dst, struct vfs_req *r)
{
    /* If the server is gone, say so. Sending and then receiving anyway would
       leave this task blocked for ever on a reply that cannot come. */
    if (sys_send(dst, r, (int)sizeof(*r)) < 0)
        return r->result = -1;
    /* From that server and nobody else. This used to be an open receive, and
       the assumption underneath it — that the next message to arrive is my
       reply — held only while a task had one thing outstanding and was not
       itself a server. It is not an assumption any more. */
    if (sys_recv_from(dst, r, (int)sizeof(*r)) < 0)
        return r->result = -1;
    return r->result;
}

/* Open a name on a server that has already been chosen. Split out because a
   union has to be walked: the caller resolves candidate 0, 1, 2 … and asks
   each in turn. */
static inline int vfs_open_at(int srv, const char *real)
{
    struct vfs_req r;
    r.op = VFS_OPEN;
    vfs__scpy(r.path, real);
    if (vfs_call(srv, &r) < 0)
        return -1;
    return (srv << 16) | (r.result & 0xffff);
}

/* Resolution answers two questions at once: who serves this name, and what
   name to serve — different questions as soon as a bind is involved, since
   /dev/console may be a connection and the server on the far end has never
   heard of that name.

   With a union there is a third: *which* of them has it. The search is here,
   in the client, rather than in the kernel, because only an open can answer
   it — the kernel holds the table but not the files. */
/* Open, or make an empty one. Same shape as open — the op is the difference. */
static inline int vfs_create(const char *path)
{
    char real[VFS_PATH_MAX];
    int srv = sys_resolve(path, real, (int)sizeof(real), 0);
    if (srv < 0)
        return -1;
    struct vfs_req r;
    r.op = VFS_CREATE;
    vfs__scpy(r.path, real);
    if (vfs_call(srv, &r) < 0)
        return -1;
    return (srv << 16) | (r.result & 0xffff);
}

static inline int vfs_open(const char *path)
{
    char real[VFS_PATH_MAX];
    for (int nth = 0; ; nth++) {
        int srv = sys_resolve(path, real, (int)sizeof(real), nth);
        if (srv < 0)
            return -1;                  /* no more members: nobody has it */
        int fd = vfs_open_at(srv, real);
        if (fd >= 0)
            return fd;
    }
}

/* The names in a directory that no server put there.

   A listing comes from whichever server answers for the directory, and that
   server has never heard of the mount table: /proc is not on the FAT16 volume
   and the filesystem is right not to mention it. Only the client knows what
   has been grafted where, because the namespace is the client's — so the
   client is where the two halves of a directory are joined. Plan 9 does this
   in its kernel, where its mount table lives; here the table is in the kernel
   but the walking of it has always been out in the library, and this is that
   same rule applied to reading a directory rather than to opening a file.

   The answer is in the ordinary listing shape, "d 0 NAME", so that a caller
   can feed it to the parser it already has. */
static inline int vfs_mounts_in(const char *dir, char *out, int cap)
{
    char buf[512];
    int n = sys_mounts(-1, buf, (int)sizeof(buf));
    if (n <= 0)
        return 0;

    char d[VFS_PATH_MAX];
    int  dl = 0;
    while (dir[dl] && dl < VFS_PATH_MAX - 1) { d[dl] = dir[dl]; dl++; }
    while (dl > 1 && d[dl - 1] == '/') dl--;        /* "/dev/" and "/dev" */
    d[dl] = 0;

    int o = 0, i = 0;
    while (i < n && o < cap - VFS_PREFIX_MAX - 8) {
        int s = i;
        while (i < n && buf[i] != '\n') i++;
        int e = i;
        if (i < n) i++;

        int pe = s;                                 /* the prefix ends at " -> " */
        while (pe < e && buf[pe] != ' ') pe++;
        int plen0 = pe - s - 1;                     /* the last character */
        while (pe > s + 1 && buf[pe - 1] == '/') pe--;

        const char *P = buf + s;
        int plen = pe - s, cut = -1;
        for (int k = 0; k < plen; k++)
            if (P[k] == '/') cut = k;
        if (cut < 0 || cut == plen - 1)
            continue;                               /* "/" names nothing in "/" */
        int parent = cut ? cut : 1;                 /* the parent of /dev is / */
        if (parent != dl)
            continue;
        int same = 1;
        for (int k = 0; k < dl; k++)
            if (P[k] != d[k]) { same = 0; break; }
        if (!same)
            continue;

        /* A prefix that ends in a slash was mounted as a directory of names
           and one that does not was mounted as a single name — /proc/ against
           /dev/console. The table records the difference; this repeats it. */
        out[o++] = buf[s + plen0] == '/' ? 'd' : '-';
        out[o++] = ' '; out[o++] = '0'; out[o++] = ' ';
        for (int k = cut + 1; k < plen; k++)
            out[o++] = P[k];
        out[o++] = '\n';
    }
    return o;
}

static inline int vfs_read(int fd, void *buf, int len)
{
    struct vfs_req r;
    if (len > VFS_DATA_MAX)
        len = VFS_DATA_MAX;
    r.op = VFS_READ; r.fd = fd & 0xffff; r.len = len;
    int n = vfs_call(fd >> 16, &r);
    if (n > 0)
        vfs__cpy(buf, r.data, n);
    return n;
}

static inline int vfs_write(int fd, const void *buf, int len)
{
    struct vfs_req r;
    if (len > VFS_DATA_MAX)
        len = VFS_DATA_MAX;
    r.op = VFS_WRITE; r.fd = fd & 0xffff; r.len = len;
    vfs__cpy(r.data, buf, len);
    return vfs_call(fd >> 16, &r);
}

/* No out-pointer any more: whatever the command yields comes home in
   `result`, because an address would not survive the trip. */
/* One message of text, with no offset — for the answers that fit in one and
   whose `len` means something else. IOCTL_HOLDS carries a task id there, and
   using the paging helper by mistake made the network server treat the id as
   an offset and cut thirteen characters off the front of its answer. */
static inline int vfs_reply_text(struct vfs_req *r, const char *text)
{
    int l = 0;
    while (text[l]) l++;
    if (l > VFS_DATA_MAX)
        l = VFS_DATA_MAX;
    vfs__cpy(r->data, text, l);
    return l;
}

/* The other half of IOCTL_DOC: what a server does to answer one. Shared here
   so that six servers do not each write it out. */
static inline int vfs_doc_reply(struct vfs_req *r, const char *text)
{
    int off = r->len > 0 ? r->len : 0;
    int l = 0;
    while (text[l]) l++;
    if (off >= l)
        return 0;                       /* the end, which is how paging stops */
    int n = l - off;
    if (n > VFS_DATA_MAX)
        n = VFS_DATA_MAX;
    vfs__cpy(r->data, text + off, n);
    return n;
}

/* An ioctl that carries a number. */
static inline int vfs_ioctl_arg(int fd, unsigned long cmd, int arg)
{
    struct vfs_req r;
    r.op = VFS_IOCTL;
    r.fd = fd & 0xffff;
    r.ioctl_cmd = cmd;
    r.len = arg;
    return vfs_call(fd >> 16, &r);
}

static inline int vfs_ioctl(int fd, unsigned long cmd)
{
    struct vfs_req r;
    r.op = VFS_IOCTL; r.fd = fd & 0xffff; r.ioctl_cmd = cmd;
    return vfs_call(fd >> 16, &r);
}

/* Where a program's output goes.

   Until now every program wrote with SYS_PUTC, which is the console of last
   resort: one character per trap, straight to the UART, needing no server and
   reaching nowhere else. That is the right thing for a startup line printed
   before anything is listening, and the wrong thing for everything after,
   because a syscall cannot be *bound* to anything. A path can. A program that
   writes to /dev/console has its output follow whatever that name means in
   its namespace — which is how a program started from the shell over TCP
   ends up talking to the connection without containing a line about it.

   The fallback is deliberate and not a nicety: if nothing is bound there, or
   if the far end has gone, the bytes still come out on the serial line rather
   than disappearing. */
static inline void vfs_say(const char *s)
{
    /* fd + 1, so that "not opened yet" is zero and this stays in .bss. A
       program whose only initialised datum is this one would otherwise grow a
       whole .data page, and these programs are loaded into a fixed buffer. */
    static int say_fd1;
    if (say_fd1 == 0) {
        int f = vfs_open("/dev/console");
        say_fd1 = f < 0 ? -1 : f + 1;
    }
    int say_fd = say_fd1 - 1;               /* -2 if the open failed */

    int n = 0;
    while (s[n])
        n++;
    if (say_fd1 < 0) {
        for (int i = 0; i < n; i++)
            _ecall1(SYS_PUTC, (unsigned char)s[i]);
        return;
    }
    /* Line endings are CR LF on the way out. A terminal reached over a
       connection is in whatever mode its owner left it, and one that does not
       translate turns a bare newline into a staircase: each line starts where
       the last one ended. RFC 854 says CR LF for exactly this reason, and the
       serial console — which adds its own CR — is unbothered by the extra
       one, since returning to column zero twice looks the same as once. */
    char buf[128];
    int off = 0, k = 0;
    for (;;) {
        if (k > (int)sizeof(buf) - 2 || (off >= n && k)) {
            int done = 0;
            while (done < k) {
                int w = vfs_write(say_fd, buf + done, k - done);
                if (w < 0) {                /* whatever it was is gone */
                    for (int i = off; i < n; i++)
                        _ecall1(SYS_PUTC, (unsigned char)s[i]);
                    return;
                }
                if (w == 0) {               /* a full send buffer: wait */
                    _ecall1(SYS_YIELD, 0);
                    continue;
                }
                done += w;
            }
            k = 0;
        }
        if (off >= n)
            return;
        if (s[off] == '\n')
            buf[k++] = '\r';
        buf[k++] = s[off++];
    }
}

/* An ioctl about a *name* rather than an open file: there is nothing to open
   when the thing does not exist yet. */
static inline int vfs_ioctl_path_arg(const char *path, unsigned long cmd,
                                     int arg)
{
    char real[VFS_PATH_MAX];
    int srv = sys_resolve(path, real, (int)sizeof(real), 0);
    if (srv < 0)
        return -1;
    struct vfs_req r;
    r.op = VFS_IOCTL;
    r.fd = -1;
    r.ioctl_cmd = cmd;
    r.len = arg;
    vfs__scpy(r.path, real);
    return vfs_call(srv, &r);
}

static inline int vfs_ioctl_path(const char *path, unsigned long cmd)
{
    return vfs_ioctl_path_arg(path, cmd, 0);
}

static inline int vfs_close(int fd)
{
    struct vfs_req r;
    r.op = VFS_CLOSE; r.fd = fd & 0xffff;
    return vfs_call(fd >> 16, &r);
}
