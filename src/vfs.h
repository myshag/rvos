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

enum { VFS_OPEN = 1, VFS_READ, VFS_WRITE, VFS_IOCTL, VFS_CLOSE };

/* Generic ioctls. IOCTL_GETSIZE reports a file's size without slurping its
   bytes. New commands plug in here without changing the transport. */
enum { IOCTL_GETSIZE = 1 };   /* answer comes back in `result` */

#define VFS_PATH_MAX 32
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
#define VFS_NNS        4        /* root + private namespaces */

struct namespace;

int vfs_mount(const char *prefix, int server_task); /* a server behind a name */
int vfs_bind(const char *old, const char *new);    /* `new` means `old` now */
int vfs_ns_clone(void);                             /* private copy of it */
/* -> the server that answers, and in `out` the name to ask it about, which
   differs from `path` if a bind was crossed. -1 if nothing is bound over it. */
int vfs_resolve(const char *path, char *out, int cap);
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
    sys_recv(r, (int)sizeof(*r));
    return r->result;
}

static inline int vfs_open(const char *path)
{
    /* Resolution answers two questions at once now: who serves this name, and
       what name to serve. They are different questions as soon as a bind is
       involved — /dev/console may be a connection, and the server on the far
       end has never heard of /dev/console. */
    char real[VFS_PATH_MAX];
    int srv = sys_resolve(path, real, (int)sizeof(real));
    if (srv < 0)
        return -1;                      /* nothing bound over this path */
    struct vfs_req r;
    r.op = VFS_OPEN;
    vfs__scpy(r.path, real);
    if (vfs_call(srv, &r) < 0)
        return -1;
    return (srv << 16) | (r.result & 0xffff);
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
    int off = 0;
    while (off < n) {
        int k = vfs_write(say_fd, s + off, n - off);
        if (k < 0) {                        /* whatever it was is gone */
            for (int i = off; i < n; i++)
                _ecall1(SYS_PUTC, (unsigned char)s[i]);
            return;
        }
        if (k == 0) {                       /* a full send buffer: wait */
            _ecall1(SYS_YIELD, 0);
            continue;
        }
        off += k;
    }
}

static inline int vfs_close(int fd)
{
    struct vfs_req r;
    r.op = VFS_CLOSE; r.fd = fd & 0xffff;
    return vfs_call(fd >> 16, &r);
}
