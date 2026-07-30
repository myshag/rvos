#pragma once
#include "riscv.h"
#include "syscall.h"
#include "util.h"

/* vfs.h — the ONE interface every module speaks, inside the kernel or out:
   open / read / write / ioctl / close. Plan-9 style: "everything is a file".
   A directory answers read() with a text listing; a device answers write()
   with a side effect; ioctl() covers whatever doesn't fit read/write.

   Crucially this is NOT a kernel syscall set — the kernel only provides
   send/recv/yield (see syscall.h). open/read/write/ioctl/close is a userspace
   *protocol*: a shared struct + a routing convention that any two tasks can
   speak on top of that raw IPC. That's the microkernel discipline: the kernel
   stays minimal; the interface is a library, not a privileged primitive.
   Adding a new module (a pipe, a network socket, a /proc) never touches the
   kernel — it's just another task that answers vfs_req the same way. */

enum { VFS_OPEN = 1, VFS_READ, VFS_WRITE, VFS_IOCTL, VFS_CLOSE };

/* One generic ioctl so far: read a file's size without slurping its bytes.
   arg is a uint32* the server writes into. More commands (seek, stat, tty
   settings, ...) plug into the same op without changing the transport. */
enum { IOCTL_GETSIZE = 1 };

#define VFS_PATH_MAX 32

struct vfs_req {
    int    op;
    int    fd;                  /* IN: server-local fd, for read/write/ioctl/close */
    char   path[VFS_PATH_MAX];  /* IN: for open */
    void  *buf;                 /* IN: read dest / write src */
    int    len;                 /* IN: requested length */
    unsigned long ioctl_cmd;
    unsigned long ioctl_arg;    /* meaning depends on ioctl_cmd */
    int    result;              /* OUT: bytes moved / local fd / 0 / -1 */
};

/* ---- static namespace: which server task owns which path prefix ----
   A real Plan 9 gives each process its own dynamically bound namespace
   (bind/mount); this teaching kernel has exactly two servers, so a fixed
   prefix table is enough. Growing this into per-task, mutable bindings is
   the natural next step once there's more than one filesystem. */
#define FS_TASK_ID      0
#define CONSOLE_TASK_ID 1

static inline int vfs_route(const char *path)
{
    static const char dev[] = "/dev/";
    int i = 0;
    for (; dev[i]; i++)
        if (path[i] != dev[i])
            return FS_TASK_ID;
    return CONSOLE_TASK_ID;
}

/* Client-visible fd packs which server owns it with that server's local fd,
   so read/write/ioctl/close never need to re-resolve the path. */
static inline int vfs_call(int dst, struct vfs_req *r)
{
    uint64 ack;
    sys_send(dst, (uint64)r);
    sys_recv(&ack);
    return r->result;
}

static inline int vfs_open(const char *path)
{
    int srv = vfs_route(path);
    struct vfs_req r;
    r.op = VFS_OPEN;
    strcpy(r.path, path);
    if (vfs_call(srv, &r) < 0)
        return -1;
    return (srv << 16) | (r.result & 0xffff);
}

static inline int vfs_read(int fd, void *buf, int len)
{
    struct vfs_req r;
    r.op = VFS_READ; r.fd = fd & 0xffff; r.buf = buf; r.len = len;
    return vfs_call(fd >> 16, &r);
}

static inline int vfs_write(int fd, const void *buf, int len)
{
    struct vfs_req r;
    r.op = VFS_WRITE; r.fd = fd & 0xffff; r.buf = (void *)buf; r.len = len;
    return vfs_call(fd >> 16, &r);
}

static inline int vfs_ioctl(int fd, unsigned long cmd, unsigned long arg)
{
    struct vfs_req r;
    r.op = VFS_IOCTL; r.fd = fd & 0xffff; r.ioctl_cmd = cmd; r.ioctl_arg = arg;
    return vfs_call(fd >> 16, &r);
}

static inline int vfs_close(int fd)
{
    struct vfs_req r;
    r.op = VFS_CLOSE; r.fd = fd & 0xffff;
    return vfs_call(fd >> 16, &r);
}
